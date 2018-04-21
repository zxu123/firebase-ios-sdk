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

#ifndef FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_EXECUTOR_H_
#define FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_EXECUTOR_H_

#include <chrono>
#include <functional>
#include <string>
#include <utility>

namespace firebase {
namespace firestore {
namespace util {
namespace internal {

using Operation = std::function<void()>;
using Milliseconds = std::chrono::milliseconds;

class ScheduledOperationHandle {
 public:
  ScheduledOperationHandle() {}
  explicit ScheduledOperationHandle(std::function<void()>&& cancel_func)
      : cancel_func_{std::move(cancel_func)} {
  }
  void Cancel() {
    cancel_func_();
  }

 private:
  std::function<void()> cancel_func_;
};

struct TaggedOperation {
  using Tag = int;
  Tag tag{};
  Operation operation;
};

class Executor {
 public:
   using Tag = TaggedOperation::Tag;

  virtual ~Executor() {
  }

  virtual void Execute(Operation&& operation) = 0;
  virtual void ExecuteBlocking(Operation&& operation) = 0;
  virtual ScheduledOperationHandle ScheduleExecution(Milliseconds delay,
                                             TaggedOperation&& operation) = 0;

  virtual bool IsAsyncCall() const = 0;
  virtual std::string GetInvokerId() const = 0;

  virtual bool IsScheduled(Tag tag) const = 0;
  virtual bool IsScheduleEmpty() const = 0;
  virtual TaggedOperation PopFromSchedule() = 0;
};

}  // namespace internal
}  // namespace util
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_EXECUTOR_H_
