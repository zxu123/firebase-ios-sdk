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

#include "Firestore/core/src/firebase/firestore/util/async_queue.h"

#include "Firestore/core/src/firebase/firestore/util/firebase_assert.h"
#include "absl/memory/memory.h"

namespace firebase {
namespace firestore {
namespace util {

// DelayedOperation

DelayedOperation::DelayedOperation() {
}

void DelayedOperation::Cancel() {
  handle_.Cancel();
}

DelayedOperation::DelayedOperation(internal::ScheduledOperationHandle&& handle)
    : handle_{std::move(handle)} {
}

// AsyncQueue

AsyncQueue::AsyncQueue(std::unique_ptr<internal::Executor> executor)
    : executor_{std::move(executor)} {
  is_operation_in_progress_ = false;
}

void AsyncQueue::VerifyIsAsyncCall() const {
  FIREBASE_ASSERT_MESSAGE(executor_->IsAsyncCall(), "TODO 1");
}

void AsyncQueue::VerifyCalledFromOperation() const {
  VerifyIsAsyncCall();  // EnsureAsync().VerifyCalledFromOperation?
  FIREBASE_ASSERT_MESSAGE(is_operation_in_progress_,
                          "VerifyCalledFromOperation called when no "
                          "operation is executing (invoker id: '%s')",
                          executor_->GetInvokerId().data());
}

void AsyncQueue::StartExecution(const Operation& operation) {
  VerifyIsAsyncCall();
  FIREBASE_ASSERT_MESSAGE(!is_operation_in_progress_,
                          "StartExecution may not be called "
                          "before the previous operation finishes");

  is_operation_in_progress_ = true;
  operation();
  is_operation_in_progress_ = false;
}

void AsyncQueue::Enqueue(const Operation& operation) {
  VerifySequentialOrder();
  EnqueueAllowingNesting(operation);
}

void AsyncQueue::EnqueueAllowingNesting(const Operation& operation) {
  // Note: can't move operation into lambda until C++14.
  executor_->Execute(Wrap(operation));
}

void AsyncQueue::EnqueueBlocking(const Operation& operation) {
  VerifySequentialOrder();
  executor_->ExecuteBlocking(Wrap(operation));
}

DelayedOperation AsyncQueue::EnqueueAfterDelay(Milliseconds delay,
                                               TimerId timer_id,
                                               Operation operation) {
  VerifyIsAsyncCall();

  // While not necessarily harmful, we currently don't expect to have multiple
  // callbacks with the same timer_id in the queue, so defensively reject
  // them.
  FIREBASE_ASSERT_MESSAGE(
      !IsScheduled(timer_id),
      "Attempted to schedule multiple operations with id %d", timer_id);

  internal::TaggedOperation tagged{static_cast<int>(timer_id), Wrap(operation)};
  auto handle = executor_->ScheduleExecution(delay, std::move(tagged));
  return DelayedOperation{std::move(handle)};
}

bool AsyncQueue::IsScheduled(const TimerId timer_id) const {
  VerifyIsAsyncCall();
  return executor_->IsScheduled(static_cast<int>(timer_id));
}

void AsyncQueue::RunScheduledOperationsUntil(const TimerId last_timer_id) {
  FIREBASE_ASSERT_MESSAGE(!executor_->IsAsyncCall(), "TODO 2");

  executor_->ExecuteBlocking([this, last_timer_id] {
    FIREBASE_ASSERT_MESSAGE(
        last_timer_id == TimerId::All || IsScheduled(last_timer_id), "TODO 3");
    FIREBASE_ASSERT_MESSAGE(!executor_->IsScheduleEmpty(), "TODO 4");
    internal::TaggedOperation tagged;
    do {
      tagged = executor_->PopFromSchedule();
      tagged.operation();
    } while (!executor_->IsScheduleEmpty() &&
             tagged.tag != static_cast<int>(last_timer_id));
  });
}

AsyncQueue::Operation AsyncQueue::Wrap(const Operation& operation) {
  return [this, operation] { StartExecution(operation); };
}

void AsyncQueue::VerifySequentialOrder() const {
  // This is the inverse of `VerifyCalledFromOperation`.
  FIREBASE_ASSERT_MESSAGE(
      !is_operation_in_progress_ || !executor_->IsAsyncCall(),
      "Enforcing sequential order failed: currently executing operations "
      "cannot enqueue nested operations (invoker id: '%s')",
      executor_->GetInvokerId().c_str());
}

}  // namespace util
}  // namespace firestore
}  // namespace firebase
