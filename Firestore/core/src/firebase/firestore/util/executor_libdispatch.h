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

#include <atomic>
#include <chrono>  // NOLINT(build/c++11)
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "dispatch/dispatch.h"

#include "Firestore/core/src/firebase/firestore/util/executor.h"
#include "Firestore/core/src/firebase/firestore/util/firebase_assert.h"
#include "absl/strings/string_view.h"

namespace firebase {
namespace firestore {
namespace util {

namespace internal {

// Generic wrapper over `dispatch_async_f`, providing `dispatch_async`-like
// interface: accepts an arbitrary invocable object in place of an Objective-C
// block.
template <typename Work>
void DispatchAsync(const dispatch_queue_t queue, Work&& work, const std::string& tag) {
  using Func = std::function<void()>;

  // Wrap the passed invocable object into a std::function. It's dynamically
  // allocated to make sure the object is valid by the time libdispatch gets to
  // it.
  struct Wrap {
    std::string tag;
    Func func;
  };
  Wrap* wrap = new Wrap{tag, work};

  dispatch_async_f(queue, wrap, [](void* const raw_work) {
    const auto unwrap = static_cast<Wrap*>(raw_work);
    unwrap->func();
    const auto copy_tag = unwrap->tag;
    (void)copy_tag;
    delete unwrap;
  });
}

// Similar to `DispatchAsync` but wraps `dispatch_sync_f`.
template <typename Work>
void DispatchSync(const dispatch_queue_t queue, Work&& work, const std::string& tag) {
  using Func = std::function<void()>;

  // Unlike dispatch_async_f, dispatch_sync_f blocks until the work passed to it
  // is done, so passing a pointer to a local variable is okay.
  struct Wrap {
    std::string tag;
    Func func;
  } wrap{tag, work};

  dispatch_sync_f(queue, &wrap, [](void* const raw_work) {
    const auto unwrap = static_cast<Wrap*>(raw_work);
    unwrap->func();
  });
}

class TimeSlot;

// A serial queue built on top of libdispatch. The operations are run on
// a dedicated serial dispatch queue.
class ExecutorLibdispatch : public Executor {
 public:
  ExecutorLibdispatch();
  explicit ExecutorLibdispatch(dispatch_queue_t dispatch_queue);

  bool IsCurrentExecutor() const override;
  std::string CurrentExecutorName() const override;

  void Execute(Operation&& operation) override;
  void ExecuteBlocking(Operation&& operation) override;
  DelayedOperation Schedule(Milliseconds delay,
                            TaggedOperation&& operation) override;

  void RemoveFromSchedule(const TimeSlot* to_remove);

  bool IsScheduled(Tag tag) const override;
  absl::optional<TaggedOperation> PopFromSchedule() override;

  void Clear() override;

  dispatch_queue_t dispatch_queue() const {
    return dispatch_queue_;
  }

 private:
  // GetLabel functions are guaranteed to never return a "null" string_view
  // (i.e. data() != nullptr).
  absl::string_view GetCurrentQueueLabel() const;
  absl::string_view GetTargetQueueLabel() const;

  // std::atomic<dispatch_queue_t> dispatch_queue_;
  dispatch_queue_t dispatch_queue_;
  // Stores non-owned pointers to `TimeSlot`s.
  // Invariant: if a `TimeSlot` is in `schedule_`, it's a valid pointer.
  std::vector<TimeSlot*> schedule_;

 public:
  std::string name_;
};

inline std::string GenerateName(const bool update = true) {
  static std::string last_name = std::string{"com.google.firebase.firestore"};
  if (update) {
    last_name = std::string{"com.google.firebase.firestore"} +
                std::to_string(std::rand());
  }
  return last_name;
}

inline ExecutorLibdispatch::ExecutorLibdispatch(const dispatch_queue_t dispatch_queue)
    : dispatch_queue_{dispatch_queue} {
  name_ = GenerateName();
}
inline ExecutorLibdispatch::ExecutorLibdispatch()
    // :
    // ExecutorLibdispatch{dispatch_queue_create("com.google.firebase.firestore",
    : ExecutorLibdispatch{dispatch_queue_create(GenerateName(true).c_str(),
                                                DISPATCH_QUEUE_SERIAL)} {
  name_ = GenerateName(false);
}

}  // namespace internal
}  // namespace util
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_UTIL_EXECUTOR_LIBDISPATCH_H_
