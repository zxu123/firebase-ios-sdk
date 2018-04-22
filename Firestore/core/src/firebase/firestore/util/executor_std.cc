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

#include "Firestore/core/src/firebase/firestore/util/executor_std.h"

namespace firebase {
namespace firestore {
namespace util {
namespace internal {

ExecutorStd::ExecutorStd() {
  // Somewhat counter-intuitively, constructor of `std::atomic` assigns the
  // value non-atomically, so the atomic initialization must be provided here,
  // before the worker thread is started.
  // See [this thread](https://stackoverflow.com/questions/25609858) for context
  // on the constructor.
  current_id_ = 0;
  shutting_down_ = false;
  worker_thread_ = std::thread{&ExecutorStd::PollingThread, this};
}

ExecutorStd::~ExecutorStd() {
  shutting_down_ = true;
  // Make sure the worker thread is not blocked, so that the call to `join`
  // doesn't hang.
  UnblockQueue();
  worker_thread_.join();
}

void ExecutorStd::Execute(Operation&& operation) {
  DoExecute(std::move(operation), Immediate());
}

DelayedOperation ExecutorStd::ScheduleExecution(
    const Milliseconds delay, TaggedOperation&& operation) {
  // While negative delay can be interpreted as a request for immediate
  // execution, supporting it would provide a hacky way to modify FIFO ordering
  // of immediate operations.
  FIREBASE_ASSERT_MESSAGE(delay.count() >= 0,
                          "ScheduleExecution: delay cannot be negative");

  namespace chr = std::chrono;

  const auto now = chr::time_point_cast<Milliseconds>(chr::system_clock::now());
  const auto id =
      DoExecute(std::move(operation.operation), now + delay, operation.tag);

  return DelayedOperation{[this, id] { TryCancel(id); }};
}

void ExecutorStd::TryCancel(const Id operation_id) {
  schedule_.RemoveIf(
      nullptr, [operation_id](const Entry& e) { return e.id == operation_id; });
}

ExecutorStd::Id ExecutorStd::DoExecute(Operation&& operation,
                                       const TimePoint when,
                                       const Tag tag) {
  // Note: operations scheduled for immediate execution don't actually need an
  // id. This could be tweaked to reuse the same id for all such operations.
  const auto id = NextId();
  schedule_.Push(Entry{std::move(operation), id, tag}, when);
  return id;
}

void ExecutorStd::PollingThread() {
  while (!shutting_down_) {
    Entry entry;
    schedule_.PopBlocking(&entry);
    if (entry.operation) {
      entry.operation();
    }
  }
}

void ExecutorStd::UnblockQueue() {
  // Put a no-op for immediate execution on the queue to ensure that
  // `schedule_.PopBlocking` returns, and worker thread can notice that shutdown
  // is in progress.
  schedule_.Push(Entry{[] {}, /*id=*/0, /*tag=*/-1}, Immediate());
}

ExecutorStd::Id ExecutorStd::NextId() {
  // The wrap around after ~4 billion operations is explicitly ignored. Even if
  // an instance of `ExecutorStd` runs long enough to get `current_id_` to
  // overflow, it's extremely unlikely that any object still holds a reference
  // that is old enough to cause a conflict.
  return current_id_++;
}

namespace {

std::string PrintThreadId(const std::thread::id thread_id) {
  const auto hashed = std::hash<std::thread::id>{}(thread_id);
  return std::to_string(hashed);
}

}  // namespace

bool ExecutorStd::IsAsyncCall() const {
  return GetInvokerId() == PrintThreadId(worker_thread_.get_id());
}

std::string ExecutorStd::GetInvokerId() const {
  return PrintThreadId(std::this_thread::get_id());
}

bool ExecutorStd::IsScheduled(const Tag tag) const {
  return {};
}

bool ExecutorStd::IsScheduleEmpty() const {
  return {};
}

TaggedOperation ExecutorStd::PopFromSchedule() {
  Entry removed;
  const bool success = schedule_.RemoveIf(
      &removed, [](const Entry& e) { return !e.IsImmediate(); });
  if (success) {
    return TaggedOperation{removed.tag, std::move(removed.operation)};
  }
  return {};
}

void ExecutorStd::ExecuteBlocking(Operation&& operation) {
}

}  // namespace internal
}  // namespace util
}  // namespace firestore
}  // namespace firebase
