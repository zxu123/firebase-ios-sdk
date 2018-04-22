/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/src/firebase/firestore/util/executor_libdispatch.h"

namespace firebase {
namespace firestore {
namespace util {
namespace internal {

namespace {

absl::string_view StringViewFromDispatchLabel(const char* const label) {
  // Make sure string_view's data is not null, because it's used for logging.
  return label ? absl::string_view{label} : absl::string_view{""};
}

}  // namespace

// TimeSlot

// All member functions, including the constructor, are *only* invoked on the
// Firestore queue. The only exception is `operator<`.
class TimeSlot {
 public:
  TimeSlot(ExecutorLibdispatch* executor,
           Milliseconds delay,
           TaggedOperation&& operation);

  void Cancel();
  TaggedOperation Unschedule();

  bool operator<(const TimeSlot& rhs) const {
    return target_time_ < rhs.target_time_;
  }
  bool operator==(const internal::TaggedOperation::Tag tag) const {
    return operation_.tag == tag;
  }

  void MarkDone() {
    done_ = true;
  }

  static void InvokedByLibdispatch(void* const raw_self);

 private:
  void Execute();
  void RemoveFromSchedule();

  using TimePoint =
      std::chrono::time_point<std::chrono::system_clock, Milliseconds>;

  ExecutorLibdispatch* const executor_;
  const TimePoint target_time_;  // Used for sorting
  TaggedOperation operation_;

  // True if the operation has either been run or canceled.
  //
  // Note on thread-safety: `done_` is only ever accessed from `Cancel` and
  // `Execute` member functions, and both of them are only ever invoked by the
  // dispatch queue, which provides synchronization.
  bool done_ = false;
};

TimeSlot::TimeSlot(ExecutorLibdispatch* const executor,
                   const Milliseconds delay,
                   TaggedOperation&& operation)
    : executor_{executor},
      target_time_{std::chrono::time_point_cast<Milliseconds>(
                       std::chrono::system_clock::now()) +
                   delay},
      operation_{std::move(operation)} {
}

void TimeSlot::Cancel() {
  if (!done_) {
    RemoveFromSchedule();
  }
}

TaggedOperation TimeSlot::Unschedule() {
  RemoveFromSchedule();
  return std::move(operation_);
}

void TimeSlot::InvokedByLibdispatch(void* const raw_self) {
  auto const self = static_cast<TimeSlot*>(raw_self);
  self->Execute();
  delete self;
}

void TimeSlot::Execute() {
  if (done_) {
    return;
  }

  RemoveFromSchedule();

  FIREBASE_ASSERT_MESSAGE(operation_.operation,
                          "TimeSlot contains an invalid function object");
  operation_.operation();
}

void TimeSlot::RemoveFromSchedule() {
  executor_->RemoveFromSchedule(this);
}

// ExecutorLibdispatch

ExecutorLibdispatch::ExecutorLibdispatch(const dispatch_queue_t dispatch_queue)
    : dispatch_queue_{dispatch_queue} {
}
ExecutorLibdispatch::ExecutorLibdispatch()
    : ExecutorLibdispatch{dispatch_queue_create("com.google.firebase.firestore",
                                                DISPATCH_QUEUE_SERIAL)} {
}

ExecutorLibdispatch::~ExecutorLibdispatch() {
  // Turn any operations that might still be in the queue into no-ops, lest
  // they try to access `ExecutorLibdispatch` after it gets destroyed.
  ExecuteBlocking([this] {
    while (!schedule_.empty()) {
      RemoveFromSchedule(schedule_.back());
    }
  });
}

bool ExecutorLibdispatch::IsAsyncCall() const {
  return GetCurrentQueueLabel().data() == GetTargetQueueLabel().data();
}
std::string ExecutorLibdispatch::GetInvokerId() const {
  return GetCurrentQueueLabel().data();
}

void ExecutorLibdispatch::Execute(Operation&& operation) {
  DispatchAsync(dispatch_queue(), std::move(operation));
}
void ExecutorLibdispatch::ExecuteBlocking(Operation&& operation) {
  DispatchSync(dispatch_queue(), std::move(operation));
}

DelayedOperation ExecutorLibdispatch::ScheduleExecution(
    Milliseconds delay, TaggedOperation&& operation) {
  namespace chr = std::chrono;
  const dispatch_time_t delay_ns = dispatch_time(
      DISPATCH_TIME_NOW, chr::duration_cast<chr::nanoseconds>(delay).count());

  // Ownership is fully transferred to libdispatch -- because it's impossible
  // to truly cancel work after it's been dispatched, libdispatch is
  // guaranteed to outlive Executor, and it's possible for work to be invoked
  // by libdispatch after Executor is destroyed. Executor only stores an
  // observer pointer to the operation.
  //
  // Invariant: if Executor contains the plain pointer to an operation, it
  // hasn't been run or canceled yet. When libdispatch invokes the operation,
  // it will remove the operation from Executor, and since it happens inside
  // `dispatch` invocation, it happens-before any access from Executor to
  // `schedule_`.

  auto const time_slot = new TimeSlot{this, delay, std::move(operation)};
  dispatch_after_f(delay_ns, dispatch_queue(), time_slot,
                   TimeSlot::InvokedByLibdispatch);
  schedule_.push_back(time_slot);
  return DelayedOperation{[this, time_slot] { RemoveFromSchedule(time_slot); }};
}

void ExecutorLibdispatch::RemoveFromSchedule(const TimeSlot* const to_remove) {
  const auto found =
      std::find_if(schedule_.begin(), schedule_.end(),
                   [to_remove](const TimeSlot* op) { return op == to_remove; });
  // It's possible for the operation to be missing if libdispatch gets to run
  // it after it was force-run, for example.
  if (found != schedule_.end()) {
    (*found)->MarkDone();
    schedule_.erase(found);
  }
}

bool ExecutorLibdispatch::IsScheduled(const Tag tag) const {
  return std::find_if(schedule_.begin(), schedule_.end(),
                      [&tag](const TimeSlot* const operation) {
                        return *operation == tag;
                      }) != schedule_.end();
}

bool ExecutorLibdispatch::IsScheduleEmpty() const {
  return schedule_.empty();
}

TaggedOperation ExecutorLibdispatch::PopFromSchedule() {
  std::sort(
      schedule_.begin(), schedule_.end(),
      [](const TimeSlot* lhs, const TimeSlot* rhs) { return *lhs < *rhs; });
  const auto nearest = schedule_.begin();
  return (*nearest)->Unschedule();
}

// GetLabel functions are guaranteed to never return a "null" string_view
// (i.e. data() != nullptr).
absl::string_view ExecutorLibdispatch::GetCurrentQueueLabel() const {
  // Note: dispatch_queue_get_label may return nullptr if the queue wasn't
  // initialized with a label.
  return StringViewFromDispatchLabel(
      dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL));
}

absl::string_view ExecutorLibdispatch::GetTargetQueueLabel() const {
  return StringViewFromDispatchLabel(
      dispatch_queue_get_label(dispatch_queue()));
}

}  // namespace internal
}  // namespace util
}  // namespace firestore
}  // namespace firebase
