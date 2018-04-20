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

#include <chrono>  // NOLINT(build/c++11)
#include <future>  // NOLINT(build/c++11)
#include <string>

#include "gtest/gtest.h"

namespace firebase {
namespace firestore {
namespace util {

namespace {

// In these generic tests the specific timer ids don't matter.
const TimerId kTimerId1 = TimerId::ListenStreamConnectionBackoff;
const TimerId kTimerId2 = TimerId::ListenStreamIdle;
const TimerId kTimerId3 = TimerId::WriteStreamConnectionBackoff;

const auto kTimeout = std::chrono::seconds(5);

class SerialQueueTest : public ::testing::Test {
 public:
  SerialQueueTest()
      : underlying_queue{dispatch_queue_create("SerialQueueTests",
                                               DISPATCH_QUEUE_SERIAL)},
        // queue{underlying_queue},
        signal_finished{[] {}} {
  }

  // Googletest doesn't contain built-in functionality to block until an async
  // operation completes, and there is no timeout by default. Work around both
  // by resolving a packaged_task in the async operation and blocking on the
  // associated future (with timeout).
  bool WaitForTestToFinish() {
    return signal_finished.get_future().wait_for(kTimeout) ==
           std::future_status::ready;
  }

  const dispatch_queue_t underlying_queue;
  SerialQueue queue;
  std::packaged_task<void()> signal_finished;
};

}  // namespace

TEST_F(SerialQueueTest, Enqueue) {
  queue.Enqueue([&] { signal_finished(); });
  EXPECT_TRUE(WaitForTestToFinish());
}

TEST_F(SerialQueueTest, EnqueueDisallowsEnqueuedTasksToUseEnqueue) {
  queue.Enqueue([&] {  // clang-format off
    // clang-format on
    EXPECT_ANY_THROW(queue.Enqueue([] {}););
    signal_finished();
  });

  EXPECT_TRUE(WaitForTestToFinish());
}

TEST_F(SerialQueueTest, EnqueueAllowsEnqueuedTasksToUseEnqueueAllowingNesting) {
  queue.Enqueue([&] {  // clang-format off
    queue.EnqueueAllowingNesting([&] { signal_finished(); });
    // clang-format on
  });

  EXPECT_TRUE(WaitForTestToFinish());
}

TEST_F(SerialQueueTest, SameQueueIsAllowedForUnownedActions) {
  dispatch_async_f(underlying_queue, this, [](void* const raw_self) {
    auto self = static_cast<SerialQueueTest*>(raw_self);
    self->queue.Enqueue([self] { self->signal_finished(); });
  });

  EXPECT_TRUE(WaitForTestToFinish());
}

TEST_F(SerialQueueTest, EnqueueBlocking) {
  bool finished = false;
  queue.EnqueueBlocking([&] { finished = true; });
  EXPECT_TRUE(finished);
}

TEST_F(SerialQueueTest, RunSyncDisallowsEnqueuedTasksToUseEnqueue) {
  queue.EnqueueBlocking([&] {  // clang-format off
    EXPECT_ANY_THROW(queue.EnqueueBlocking([] {}););
    // clang-format on
  });
}

TEST_F(SerialQueueTest, StartExecutionDisallowsNesting) {
  queue.EnqueueBlocking(
      [&] { EXPECT_ANY_THROW(queue.StartExecution([] {});); });
}

TEST_F(SerialQueueTest, VerifyCalledFromOperationRequiresBeingCalledAsync) {
  ASSERT_NE(underlying_queue, dispatch_get_main_queue());
  EXPECT_ANY_THROW(queue.VerifyCalledFromOperation());
}

TEST_F(SerialQueueTest, VerifyCalledFromOperationRequiresOperationInProgress) {
  dispatch_sync_f(underlying_queue, &queue, [](void* const raw_queue) {
    EXPECT_ANY_THROW(
        static_cast<SerialQueue*>(raw_queue)->VerifyCalledFromOperation());
  });
}

TEST_F(SerialQueueTest, VerifyCalledFromOperationWorksWithOperationInProgress) {
  queue.EnqueueBlocking(
      [&] { EXPECT_NO_THROW(queue.VerifyCalledFromOperation()); });
}

TEST_F(SerialQueueTest, CanScheduleOperationsInTheFuture) {
  std::string steps;

  queue.Enqueue([&steps] { steps += '1'; });
  queue.Enqueue([&] {
    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(5), kTimerId1, [&] {
      steps += '4';
      signal_finished();
    });
    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(1), kTimerId2,
                            [&steps] { steps += '3'; });
    queue.EnqueueAllowingNesting([&steps] { steps += '2'; });
  });

  EXPECT_TRUE(WaitForTestToFinish());
  EXPECT_EQ(steps, "1234");
}

TEST_F(SerialQueueTest, CanCancelDelayedCallbacks) {
  std::string steps;

  queue.Enqueue([&] {
    // Queue everything from the queue to ensure nothing completes before we
    // cancel.

    queue.EnqueueAllowingNesting([&steps] { steps += '1'; });

    DelayedOperation delayed_operation = queue.EnqueueAfterDelay(
        SerialQueue::Milliseconds(1), kTimerId1, [&steps] { steps += '2'; });

    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(5), kTimerId2, [&] {
      steps += '3';
      signal_finished();
    });

    // EXPECT_TRUE(queue.ContainsDelayedOperation(kTimerId1));
    delayed_operation.Cancel();
    // EXPECT_FALSE(queue.ContainsDelayedOperation(kTimerId1));
  });

  EXPECT_TRUE(WaitForTestToFinish());
  EXPECT_EQ(steps, "13");
  // queue.EnqueueBlocking(
  //     [&] { EXPECT_FALSE(queue.ContainsDelayedOperation(kTimerId1)); });
}

TEST_F(SerialQueueTest, DelayedOperationIsValidAfterTheOperationHasRun) {
  DelayedOperation delayed_operation;
  queue.Enqueue([&] {
    delayed_operation = queue.EnqueueAfterDelay(
        SerialQueue::Milliseconds(10), kTimerId1, [&] { signal_finished(); });
    // EXPECT_TRUE(queue.ContainsDelayedOperation(kTimerId1));
  });

  EXPECT_TRUE(WaitForTestToFinish());
  // queue.EnqueueBlocking(
      // [&] { EXPECT_FALSE(queue.ContainsDelayedOperation(kTimerId1)); });
  EXPECT_NO_THROW(delayed_operation.Cancel());
}

TEST_F(SerialQueueTest, CanManuallyDrainAllDelayedCallbacksForTesting) {
  std::string steps;

  queue.Enqueue([&] {
    queue.EnqueueAllowingNesting([&steps] { steps += '1'; });
    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(20000), kTimerId1,
                            [&] { steps += '4'; });
    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(10000), kTimerId2,
                            [&steps] { steps += '3'; });
    queue.EnqueueAllowingNesting([&steps] { steps += '2'; });
    signal_finished();
  });

  EXPECT_TRUE(WaitForTestToFinish());
  // queue.RunDelayedOperationsUntil(TimerId::All);
  EXPECT_EQ(steps, "1234");
}

TEST_F(SerialQueueTest, CanManuallyDrainSpecificDelayedCallbacksForTesting) {
  std::string steps;

  queue.Enqueue([&] {
    queue.EnqueueAllowingNesting([&] { steps += '1'; });
    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(20000), kTimerId1,
                            [&steps] { steps += '5'; });
    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(10000), kTimerId2,
                            [&steps] { steps += '3'; });
    queue.EnqueueAfterDelay(SerialQueue::Milliseconds(15000), kTimerId3,
                            [&steps] { steps += '4'; });
    queue.EnqueueAllowingNesting([&] { steps += '2'; });
    signal_finished();
  });

  EXPECT_TRUE(WaitForTestToFinish());
  // queue.RunDelayedOperationsUntil(kTimerId3);
  EXPECT_EQ(steps, "1234");
}

}  // namespace util
}  // namespace firestore
}  // namespace firebase
