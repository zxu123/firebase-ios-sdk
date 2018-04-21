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

#include "Firestore/core/src/firebase/firestore/util/executor.h"

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

class DelayedOperation {
 public:
  DelayedOperation();

  void Cancel();

 private:
  // Don't allow callers to create their own valid `DelayedOperation`s.
  friend class AsyncQueue;
  explicit DelayedOperation(internal::ScheduledOperationHandle&& handle);

  internal::ScheduledOperationHandle handle_;
};

class AsyncQueue {
 public:
  using Milliseconds = internal::Milliseconds;
  using Operation = internal::Operation;
  using ExecutorT = internal::Executor<TimerId>;

  explicit AsyncQueue(
      std::unique_ptr<ExecutorT> executor);

  void VerifyIsAsyncCall() const;
  void VerifyCalledFromOperation() const;

  void Enqueue(const Operation& operation);
  void EnqueueAllowingNesting(const Operation& operation);
  DelayedOperation EnqueueAfterDelay(Milliseconds delay,
                                     TimerId timer_id,
                                     Operation operation);

  void StartExecution(const Operation& operation);

  void EnqueueBlocking(const Operation& operation);
  bool IsScheduled(const TimerId timer_id) const;
  void RunScheduledOperationsUntil(const TimerId last_timer_id);

 private:
  using TaggedOperationT = internal::TaggedOperation<TimerId>;

  Operation Wrap(const Operation& operation);

  void VerifySequentialOrder() const;

  std::atomic<bool> is_operation_in_progress_;
  std::unique_ptr<ExecutorT> executor_;
};

}  // namespace util
}  // namespace firestore
}  // namespace firebase

// TODO
// dispatch_queue_t dispatch_queue() const;

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_H_
