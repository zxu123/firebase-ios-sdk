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

#ifndef FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_EXECUTOR_LIBDISPATCH_H_
#define FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_EXECUTOR_LIBDISPATCH_H_

#include <dispatch/dispatch.h>
#include <atomic>
#include <chrono>  // NOLINT(build/c++11)
#include <functional>
#include <memory>
#include <vector>

#include "Firestore/core/src/firebase/firestore/util/firebase_assert.h"
#include "absl/strings/string_view.h"

namespace firebase {
namespace firestore {
namespace util {

absl::string_view StringViewFromLabel(const char* const label) {
  // Make sure string_view's data is not null, because it's used for logging.
  return label ? absl::string_view{label} : absl::string_view{""};
}

namespace internal {

template <typename Callable>
class Executor;
using Func = std::function<void()>;
using Milliseconds = std::chrono::milliseconds;

// All member functions, including the constructor, are *only* invoked on the
// Firestore queue. The only exception is `operator<`.
template <typename Callable>
class ScheduledOperation {
 public:
  ScheduledOperation(Executor<Callable>* const executor,
                     const Milliseconds delay,
                     Callable&& callable)
      : executor_{executor},
        target_time_{std::chrono::time_point_cast<Milliseconds>(
                         std::chrono::system_clock::now()) +
                     delay},
        callable_{std::move(callable)} {
  }

  void Cancel();

  Callable Unschedule() {
    RemoveFromSchedule();
    return std::move(callable_);
  }

  bool operator<(const ScheduledOperation& rhs) const {
    return target_time_ < rhs.target_time_;
  }
  bool operator==(const Callable& rhs) const {
    return callable_ == rhs;
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

  Executor<Callable>* const executor_;
  const TimePoint target_time_;  // Used for sorting
  Callable callable_;

  // True if the operation has either been run or canceled.
  //
  // Note on thread-safety: `done_` is only ever accessed from `Cancel` and
  // `Execute` member functions, and both of them are only ever invoked by the
  // dispatch queue, which provides synchronization.
  bool done_ = false;
};

// Wrappers

// Generic wrapper over dispatch_async_f, providing dispatch_async-like
// interface: accepts an arbitrary invocable object in place of an Objective-C
// block.
template <typename Work>
void DispatchAsync(const dispatch_queue_t queue, Work&& work) {
  // Wrap the passed invocable object into a std::function. It's dynamically
  // allocated to make sure the object is valid by the time libdispatch gets to
  // it.
  const auto wrap = new Func(std::forward<Work>(work));

  dispatch_async_f(queue, wrap, [](void* const raw_operation) {
    const auto unwrap = static_cast<Func*>(raw_operation);
    (*unwrap)();
    delete unwrap;
  });
}

// Similar to DispatchAsync but wraps dispatch_sync_f.
template <typename Work>
void DispatchSync(const dispatch_queue_t queue, Work&& work) {
  // Unlike dispatch_async_f, dispatch_sync_f blocks until the work passed to it
  // is done, so passing a pointer to a local variable is okay.
  Func wrap{std::forward<Work>(work)};

  dispatch_sync_f(queue, &wrap, [](void* const raw_operation) {
    const auto unwrap = static_cast<Func*>(raw_operation);
    (*unwrap)();
  });
}

// Executor

template <typename Callable>
class Executor {
 public:
  explicit Executor(const dispatch_queue_t dispatch_queue)
      : dispatch_queue_{dispatch_queue} {
  }
  Executor()
      : Executor{dispatch_queue_create("com.google.firebase.firestore",
                                       DISPATCH_QUEUE_SERIAL)} {
  }

  ~Executor() {
    // Turn any operations that might still be in the queue into no-ops, lest
    // they try to access `Executor` after it gets destroyed.
    ExecuteBlocking([this] {
      while (!schedule_.empty()) {
        RemoveFromSchedule(schedule_.back());
      }
    });
  }

  bool IsAsyncCall() const {
    return GetCurrentQueueLabel().data() == GetTargetQueueLabel().data();
  }
  absl::string_view GetInvokerId() const {
    return GetCurrentQueueLabel();
  }

  void Execute(Func operation) {
    DispatchAsync(dispatch_queue(), std::move(operation));
  }
  void ExecuteBlocking(Func operation) {
    DispatchSync(dispatch_queue(), std::move(operation));
  }

  ScheduledOperation<Callable>* ScheduleExecution(Milliseconds delay,
                                                  Callable callable) {
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

    auto const delayed_operation =
        new ScheduledOperation<Callable>{this, delay, std::move(callable)};
    dispatch_after_f(delay_ns, dispatch_queue(), delayed_operation,
                     ScheduledOperation<Callable>::InvokedByLibdispatch);
    schedule_.push_back(delayed_operation);
    return delayed_operation;
  }

  void RemoveFromSchedule(const ScheduledOperation<Callable>* const to_remove) {
    const auto found =
        std::find_if(schedule_.begin(), schedule_.end(),
                     [to_remove](const ScheduledOperation<Callable>* op) {
                       return op == to_remove;
                     });
    // It's possible for the operation to be missing if libdispatch gets to run
    // it after it was force-run, for example.
    if (found != schedule_.end()) {
      (*found)->MarkDone();
      schedule_.erase(found);
    }
  }

  bool IsScheduled(const Callable& callable) const {
    return std::find_if(
               schedule_.begin(), schedule_.end(),
               [&callable](
                   const ScheduledOperation<Callable>* const operation) {
                 return *operation == callable;
               }) != schedule_.end();
  }

  bool IsScheduleEmpty() const {
    return schedule_.empty();
  }

  Callable PopFromSchedule() {
    std::sort(
        schedule_.begin(), schedule_.end(),
        [](const ScheduledOperation<Callable>* lhs,
           const ScheduledOperation<Callable>* rhs) { return *lhs < *rhs; });
    const auto nearest = schedule_.begin();
    return (*nearest)->Unschedule();
  }

 private:
  dispatch_queue_t dispatch_queue() const {
    return dispatch_queue_;
  }

  // GetLabel functions are guaranteed to never return a "null" string_view
  // (i.e. data() != nullptr).
  absl::string_view GetCurrentQueueLabel() const {
    // Note: dispatch_queue_get_label may return nullptr if the queue wasn't
    // initialized with a label.
    return StringViewFromLabel(
        dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL));
  }

  absl::string_view GetTargetQueueLabel() const {
    return StringViewFromLabel(dispatch_queue_get_label(dispatch_queue()));
  }

  std::atomic<dispatch_queue_t> dispatch_queue_;
  std::vector<ScheduledOperation<Callable>*> schedule_;
};

template <typename Callable>
void ScheduledOperation<Callable>::Cancel() {
  if (!done_) {
    RemoveFromSchedule();
  }
}

template <typename Callable>
void ScheduledOperation<Callable>::InvokedByLibdispatch(void* const raw_self) {
  auto const self = static_cast<ScheduledOperation*>(raw_self);
  self->Execute();
  delete self;
}

template <typename Callable>
void ScheduledOperation<Callable>::Execute() {
  if (done_) {
    return;
  }

  RemoveFromSchedule();

  FIREBASE_ASSERT_MESSAGE(callable_,
                          "ScheduledOperation contains an invalid callable");
  callable_();
}

template <typename Callable>
void ScheduledOperation<Callable>::RemoveFromSchedule() {
  executor_->RemoveFromSchedule(this);
}

}  // namespace internal
}  // namespace util
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_EXECUTOR_LIBDISPATCH_H_
