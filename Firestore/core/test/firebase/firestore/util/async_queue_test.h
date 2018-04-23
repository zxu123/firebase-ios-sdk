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

#ifndef FIRESTORE_CORE_TEST_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_TEST_H_
#define FIRESTORE_CORE_TEST_FIREBASE_FIRESTORE_UTIL_ASYNC_QUEUE_TEST_H_

#include <future>  // NOLINT(build/c++11)
#include <memory>

#include "gtest/gtest.h"

#include "Firestore/core/src/firebase/firestore/util/async_queue.h"

namespace firebase {
namespace firestore {
namespace util {

class AsyncQueueTest : public ::testing::TestWithParam<internal::Executor*> {
 public:
  AsyncQueueTest()
      : queue{std::unique_ptr<internal::Executor>(GetParam())},
        signal_finished{[] {}} {
  }

  // Googletest doesn't contain built-in functionality to block until an async
  // operation completes, and there is no timeout by default. Work around both
  // by resolving a packaged_task in the async operation and blocking on the
  // associated future (with timeout).
  bool WaitForTestToFinish() {
    const auto kTimeout = std::chrono::seconds(5);
    return signal_finished.get_future().wait_for(kTimeout) ==
           std::future_status::ready;
  }


  AsyncQueue queue;
  std::packaged_task<void()> signal_finished;
};

}  // namespace util
}  // namespace firestore
}  // namespace firebase

#endif
