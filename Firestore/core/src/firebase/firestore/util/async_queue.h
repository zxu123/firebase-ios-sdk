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

#ifndef FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_H_
#define FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_H_

#include <atomic>
#include <chrono>  // NOLINT(build/c++11)
#include <functional>
#include <memory>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"

#include "Firestore/core/src/firebase/firestore/util/executor_libdispatch.h"
#include "Firestore/core/src/firebase/firestore/util/firebase_assert.h"

namespace firebase {
namespace firestore {
namespace util {

/**
 * Well-known "timer" ids used when scheduling delayed operations on the
 * AsyncQueue. These ids can then be used from tests to check for the
 * presence of delayed operations or to run them early.
 */
enum class TimerId {
  /** All can be used with `RunDelayedOperationsUntil` to run all timers. */
  All,

  /**
   * The following 4 timers are used in `Stream` for the listen and write
   * streams. The "Idle" timer is used to close the stream due to inactivity.
   * The "ConnectionBackoff" timer is used to restart a stream once the
   * appropriate backoff delay has elapsed.
   */
  ListenStreamIdle,
  ListenStreamConnectionBackoff,
  WriteStreamIdle,
  WriteStreamConnectionBackoff,

  /**
   * A timer used in `OnlineStateTracker` to transition from
   * `OnlineStateUnknown` to `Offline` after a set timeout, rather than waiting
   * indefinitely for success or failure.
   */
  OnlineStateTimeout,
};

namespace internal {

template <typename Callable>
class Executor;
template <typename Callable>
class ScheduledOperation;

struct TaggedOperation {
  void operator()() const {
    func();
  }
  explicit operator bool() const {
    return static_cast<bool>(func);
  }
  bool operator==(const TaggedOperation& rhs) const {
    return tag == rhs.tag;
  }

  TimerId tag;
  std::function<void()> func;
};

}  // namespace internal

class SerialQueue;

class DelayedOperation {
 public:
  DelayedOperation() {
  }

  void Cancel() {
    cancel_();
  }

 private:
  // Don't allow callers to create their own valid `DelayedOperation`s.
  friend class SerialQueue;
  explicit DelayedOperation(std::function<void()>&& cancel)
      : cancel_{std::move(cancel)} {
  }

  std::function<void()> cancel_;
};

class SerialQueue {
 public:
  using Milliseconds = std::chrono::milliseconds;
  using Operation = std::function<void()>;

  SerialQueue()
      : SerialQueue{absl::make_unique<
            internal::Executor<internal::TaggedOperation>>()} {
  }
  explicit SerialQueue(
      std::unique_ptr<internal::Executor<internal::TaggedOperation>> executor)
      : executor_{std::move(executor)} {
    is_operation_in_progress_ = false;
  }

  void VerifyCalledFromOperation() const {
    VerifyIsAsyncCall();  // EnsureAsync().VerifyCalledFromOperation?
    FIREBASE_ASSERT_MESSAGE(is_operation_in_progress_,
                            "VerifyCalledFromOperation called when no "
                            "operation is executing (invoker id: '%s')",
                            executor_->GetInvokerId().data());
  }

  void StartExecution(const Operation& operation) {
    VerifyIsAsyncCall();
    FIREBASE_ASSERT_MESSAGE(!is_operation_in_progress_,
                            "StartExecution may not be called "
                            "before the previous operation finishes");

    is_operation_in_progress_ = true;
    operation();
    is_operation_in_progress_ = false;
  }

  void Enqueue(const Operation& operation) {
    VerifySequentialOrder();
    EnqueueAllowingNesting(operation);
  }
  void EnqueueAllowingNesting(const Operation& operation) {
    // Note: can't move operation into lambda until C++14.
    executor_->Execute(Wrap(operation));
  }
  void EnqueueBlocking(const Operation& operation) {
    VerifySequentialOrder();
    executor_->ExecuteBlocking(Wrap(operation));
  }

  DelayedOperation EnqueueAfterDelay(Milliseconds delay,
                                     TimerId timer_id,
                                     Operation operation) {
    VerifyIsAsyncCall();

    // While not necessarily harmful, we currently don't expect to have multiple
    // callbacks with the same timer_id in the queue, so defensively reject
    // them.
    FIREBASE_ASSERT_MESSAGE(
        !IsScheduled(timer_id),
        "Attempted to schedule multiple operations with id %d", timer_id);

    internal::TaggedOperation tagged_operation{timer_id, Wrap(operation)};
    const auto operation_impl =
        executor_->ScheduleExecution(delay, std::move(tagged_operation));
    return DelayedOperation{[this, operation_impl] { Cancel(operation_impl); }};
  }

  void VerifyIsAsyncCall() const {
    FIREBASE_ASSERT_MESSAGE(executor_->IsAsyncCall(), "TODO 1");
  }

  bool IsScheduled(const TimerId timer_id) const {
    VerifyIsAsyncCall();
    return executor_->IsScheduled(internal::TaggedOperation{timer_id, nullptr});
  }

  void RunScheduledOperationsUntil(const TimerId last_timer_id) {
    FIREBASE_ASSERT_MESSAGE(!executor_->IsAsyncCall(), "TODO 2");

    executor_->ExecuteBlocking([this, last_timer_id] {
      FIREBASE_ASSERT_MESSAGE(
          last_timer_id == TimerId::All || IsScheduled(last_timer_id),
          "TODO 3");
      FIREBASE_ASSERT_MESSAGE(!executor_->IsScheduleEmpty(), "TODO 4");
      internal::TaggedOperation o;
      do {
        o = executor_->PopFromSchedule();
        o.func();
      } while (!executor_->IsScheduleEmpty() && o.tag != last_timer_id);
    });
  }

 private:
  void Cancel(const internal::ScheduledOperation<internal::TaggedOperation>*
                  to_cancel) {
    VerifyIsAsyncCall();
    executor_->RemoveFromSchedule(to_cancel);
  }

  Operation Wrap(const Operation& operation) {
    return [this, operation] { StartExecution(operation); };
  }

  void VerifySequentialOrder() const {
    // This is the inverse of `VerifyCalledFromOperation`.
    FIREBASE_ASSERT_MESSAGE(
        !is_operation_in_progress_ || !executor_->IsAsyncCall(),
        "Enforcing sequential order failed: currently executing operations "
        "cannot enqueue nested operations (invoker id: '%s')",
        executor_->GetInvokerId().data());
  }

  std::atomic<bool> is_operation_in_progress_;
  std::unique_ptr<internal::Executor<internal::TaggedOperation>> executor_;
};

}  // namespace util
}  // namespace firestore
}  // namespace firebase

// TODO
// dispatch_queue_t dispatch_queue() const;

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_H_
