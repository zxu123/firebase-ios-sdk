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

#include "Firestore/core/src/firebase/firestore/util/async_queue_libdispatch.h"

#include <algorithm>
#include <utility>

namespace firebase {
namespace firestore {
namespace util {

namespace internal {

/*
bool AsyncQueueImpl::ContainsDelayedOperation(const TimerId timer_id) const {
  VerifyOnTargetQueue();
  return std::find_if(operations_.begin(), operations_.end(),
                      [timer_id](const DelayedOperationPtr& op) {
                        return op->timer_id() == timer_id;
                      }) != operations_.end();
}

void AsyncQueueImpl::RunDelayedOperationsUntil(const TimerId last_timer_id) {
  const dispatch_semaphore_t done_semaphore = dispatch_semaphore_create(0);

  Enqueue([this, last_timer_id, done_semaphore] {
    std::sort(
        operations_.begin(), operations_.end(),
        [](const DelayedOperationPtr& lhs, const DelayedOperationPtr& rhs) {
          return lhs->operator<(*rhs);
        });

    const auto until = [this, last_timer_id] {
      if (last_timer_id == TimerId::All) {
        return operations_.end();
      }
      const auto found =
          std::find_if(operations_.begin(), operations_.end(),
                       [last_timer_id](const DelayedOperationPtr& op) {
                         return op->timer_id() == last_timer_id;
                       });
      FIREBASE_ASSERT_MESSAGE(
          found != operations_.end(),
          "Attempted to run operations until missing timer id: %d",
          last_timer_id);
      return found + 1;
    }();

    for (auto it = operations_.begin(); it != until; ++it) {
      (*it)->RescheduleAsap();
    }

    // Now that the callbacks are queued, we want to enqueue an additional item
    // to release the 'done' semaphore.
    EnqueueAllowingSameQueue(
        [done_semaphore] { dispatch_semaphore_signal(done_semaphore); });
  });

  dispatch_semaphore_wait(done_semaphore, DISPATCH_TIME_FOREVER);
}
*/

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

namespace {

absl::string_view StringViewFromLabel(const char* const label) {
  // Make sure string_view's data is not null, because it's used for logging.
  return label ? absl::string_view{label} : absl::string_view{""};
}

}  // namespace

// DelayedOperationImpl

// All member functions, including the constructor, are *only* invoked on the
// Firestore queue. The only exception is `operator<`.
class DelayedOperationImpl {
 public:
  DelayedOperationImpl(Executor* const executor_,
                       const AsyncQueue::Milliseconds delay,
                       AsyncQueue::Operation&& operation)
      : executor_{executor} target_time_{
            std::chrono::time_point_cast<AsyncQueue::Milliseconds>(
                std::chrono::system_clock::now()) +
            delay},
        operation_{std::move(operation)} {
  }

  void Cancel() {
    if (!done_) {
      Dequeue();
    }
  }

  void RescheduleAsap() {
    FIREBASE_ASSERT_MESSAGE(!done_, "TODO");
    executor_->Enqueue([this] { Execute(); });
  }

  bool operator<(const DelayedOperationImpl& rhs) const {
    return target_time_ < rhs.target_time_;
  }

  void MarkDone() {
    done_ = true;
  }

 private:
  static void InvokedByLibdispatch(void* const raw_self) {
    auto const self = static_cast<DelayedOperationImpl*>(raw_self);
    self->executor_->StartExecution([self] { self->Execute(); });
    // StartExecution is a blocking operation.
    delete self;
  }

  void Execute() {
    if (done_) {
      return;
    }

    Dequeue();
    MarkDone();

    FIREBASE_ASSERT_MESSAGE(
        operation_, "DelayedOperationImpl contains invalid function object");
    operation_();
  }

  void Dequeue() {
    executor_->Remove(*this);
  }

  using TimePoint = std::chrono::time_point<std::chrono::system_clock,
                                            AsyncQueue::Milliseconds>;

  Executor* const executor_;
  const TimePoint target_time_;  // Used for sorting
  const AsyncQueue::Operation operation_;

  // True if the operation has either been run or canceled.
  //
  // Note on thread-safety: `done_` is only ever accessed from `Cancel` and
  // `Execute` member functions, and both of them are only ever invoked by the
  // dispatch queue, which provides synchronization.
  bool done_ = false;
};

// Executor

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

  DelayedOperation* ScheduleExecution(Milliseconds delay,
                                     TimerId timer_id,
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
        new DelayedOperationImpl{this, delay, std::move(operation)};
    dispatch_after_f(delay_ns, dispatch_queue(), delayed_operation,
                     DelayedOperationImpl::InvokedByLibdispatch);
    operations_.push_back({timer_id, delayed_operation});
    return delayed_operation;
  }

  void Remove(const DelayedOperationImpl& to_remove) {
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
    DelayedOperationImpl* op_{};
  };
  std::vector<TaggedOperation> operations_;
};

}  // namespace internal

}  // namespace util
}  // namespace firestore
}  // namespace firebase
