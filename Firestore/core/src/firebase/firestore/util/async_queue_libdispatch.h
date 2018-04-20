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

#ifndef FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_LIBDISPATCH_H_
#define FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_LIBDISPATCH_H_

#include <dispatch/dispatch.h>
#include <atomic>
#include <chrono>  // NOLINT(build/c++11)
#include <functional>
#include <memory>
#include <vector>

#include "absl/strings/string_view.h"
#include "Firestore/core/src/firebase/firestore/util/firebase_assert.h"

namespace firebase {
namespace firestore {
namespace util {

namespace {

absl::string_view StringViewFromLabel(const char* const label) {
  // Make sure string_view's data is not null, because it's used for logging.
  return label ? absl::string_view{label} : absl::string_view{""};
}

}  // namespace

// DelayedOperationImpl

namespace internal {

template <typename Tag> class Executor;
using Operation = std::function<void()>;
using Milliseconds = std::chrono::milliseconds;

// All member functions, including the constructor, are *only* invoked on the
// Firestore queue. The only exception is `operator<`.
template <typename Tag>
class DelayedOperationImpl {
 public:
DelayedOperationImpl(Executor<Tag>* const executor,
                       const Milliseconds delay,
                       Operation&& operation)
      : executor_{executor}, target_time_{
            std::chrono::time_point_cast<Milliseconds>(
                std::chrono::system_clock::now()) +
            delay},
        operation_{std::move(operation)} {
  }

  void Cancel();
  void RescheduleAsap();
  bool operator<(const DelayedOperationImpl& rhs) const {
    return target_time_ < rhs.target_time_;
  }

  void MarkDone() {
    done_ = true;
  }

 private:
  static void InvokedByLibdispatch(void* const raw_self);
  void Execute();
  void Dequeue();

  using TimePoint = std::chrono::time_point<std::chrono::system_clock,
                                            Milliseconds>;

  Executor<Tag>* const executor_;
  const TimePoint target_time_;  // Used for sorting
  const Operation operation_;

  // True if the operation has either been run or canceled.
  //
  // Note on thread-safety: `done_` is only ever accessed from `Cancel` and
  // `Execute` member functions, and both of them are only ever invoked by the
  // dispatch queue, which provides synchronization.
  bool done_ = false;
};

// Executor

// Generic wrapper over dispatch_async_f, providing dispatch_async-like
// interface: accepts an arbitrary invocable object in place of an Objective-C
// block.
template <typename Work>
void DispatchAsync(const dispatch_queue_t queue, Work&& work) {
  // Wrap the passed invocable object into a std::function. It's dynamically
  // allocated to make sure the object is valid by the time libdispatch gets to
  // it.
  const auto wrap = new Operation(std::forward<Work>(work));

  dispatch_async_f(queue, wrap, [](void* const raw_operation) {
    const auto unwrap = static_cast<Operation*>(raw_operation);
    (*unwrap)();
    delete unwrap;
  });
}

// Similar to DispatchAsync but wraps dispatch_sync_f.
template <typename Work>
void DispatchSync(const dispatch_queue_t queue, Work&& work) {
  // Unlike dispatch_async_f, dispatch_sync_f blocks until the work passed to it
  // is done, so passing a pointer to a local variable is okay.
  Operation wrap{std::forward<Work>(work)};

  dispatch_sync_f(queue, &wrap, [](void* const raw_operation) {
    const auto unwrap = static_cast<Operation*>(raw_operation);
    (*unwrap)();
  });
}


template <typename Tag>
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
      for (auto operation : operations_) {
        operation->MarkDone();
      }
    });
  }

  bool IsAsyncCall() const {
    return GetCurrentQueueLabel().data() == GetTargetQueueLabel().data();
  }
  absl::string_view GetInvokerId() const {
    return GetCurrentQueueLabel();
  }

  void Execute(Operation operation) {
    DispatchAsync(dispatch_queue(), std::move(operation));
  }
  void ExecuteBlocking(Operation operation) {
    DispatchSync(dispatch_queue(), std::move(operation));
  }

  DelayedOperationImpl<Tag>* ScheduleExecution(Milliseconds delay,
                                     Tag tag,
                                     Operation operation) {
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
    // `operations_`.

    auto const delayed_operation =
        new DelayedOperationImpl<Tag>{this, delay, std::move(operation)};
    dispatch_after_f(delay_ns, dispatch_queue(), delayed_operation,
                     DelayedOperationImpl<Tag>::InvokedByLibdispatch);
    operations_.push_back({tag, delayed_operation});
    return delayed_operation;
  }

  void Remove(const DelayedOperationImpl<Tag>& to_remove) {
    const auto found = std::find_if(operations_.begin(), operations_.end(),
                                    [&to_remove](const TaggedOperation& op) {
                                      return op.operation == &to_remove;
                                    });
    // It's possible for the operation to be missing if libdispatch gets to run
    // it after it was force-run, for example.
    if (found != operations_.end()) {
      operations_.erase(found);
    }
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
  struct TaggedOperation {
    Tag tag{};
    DelayedOperationImpl<Tag>* op_{};
  };
  std::vector<TaggedOperation> operations_;
};

template <typename Tag>
void DelayedOperationImpl<Tag>::Cancel() {
  if (!done_) {
    Dequeue();
  }
}

template <typename Tag>
void DelayedOperationImpl<Tag>::RescheduleAsap() {
  FIREBASE_ASSERT_MESSAGE(!done_, "TODO");
  executor_->Enqueue([this] { Execute(); });
}

template <typename Tag>
void DelayedOperationImpl<Tag>::InvokedByLibdispatch(void* const raw_self) {
  auto const self = static_cast<DelayedOperationImpl*>(raw_self);
  self->executor_->StartExecution([self] { self->Execute(); });
  // StartExecution is a blocking operation.
  delete self;
}

template <typename Tag>
void DelayedOperationImpl<Tag>::Execute() {
  if (done_) {
    return;
  }

  Dequeue();
  MarkDone();

  FIREBASE_ASSERT_MESSAGE(
      operation_, "DelayedOperationImpl contains invalid function object");
  operation_();
}

template <typename Tag>
void DelayedOperationImpl<Tag>::Dequeue() {
  executor_->Remove(*this);
}

}  // namespace internal
}  // namespace util
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_LIBDISPATCH_H_
