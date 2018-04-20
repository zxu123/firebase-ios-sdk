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

#include "Firestore/core/src/firebase/firestore/util/firebase_assert.h"

namespace firebase {
namespace firestore {
namespace util {

namespace {

// Generic wrapper over dispatch_async_f, providing dispatch_async-like
// interface: accepts an arbitrary invocable object in place of an Objective-C
// block.
template <typename Work>
void DispatchAsync(const dispatch_queue_t queue, Work&& work) {
  // Wrap the passed invocable object into a std::function. It's dynamically
  // allocated to make sure the object is valid by the time libdispatch gets to
  // it.
  const auto wrap = new AsyncQueue::Operation(std::forward<Work>(work));

  dispatch_async_f(queue, wrap, [](void* const raw_operation) {
    const auto unwrap = static_cast<AsyncQueue::Operation*>(raw_operation);
    (*unwrap)();
    delete unwrap;
  });
}

// Similar to DispatchAsync but wraps dispatch_sync_f.
template <typename Work>
void DispatchSync(const dispatch_queue_t queue, Work&& work) {
  // Unlike dispatch_async_f, dispatch_sync_f blocks until the work passed to it
  // is done, so passing a pointer to a local variable is okay.
  AsyncQueue::Operation wrap{std::forward<Work>(work)};

  dispatch_sync_f(queue, &wrap, [](void* const raw_operation) {
    const auto unwrap = static_cast<AsyncQueue::Operation*>(raw_operation);
    (*unwrap)();
  });
}

}  // namespace

namespace internal {

class AsyncQueueImpl {
 public:
  using Milliseconds = AsyncQueue::Milliseconds;
  using Operation = AsyncQueue::Operation;

  explicit AsyncQueueImpl(dispatch_queue_t dispatch_queue);

  void VerifyIsCurrentQueue() const;
  void EnterCheckedOperation(const Operation& operation);

  void Enqueue(const Operation& operation);
  void EnqueueAllowingSameQueue(const Operation& operation);

  DelayedOperation EnqueueAfterDelay(Milliseconds delay,
                                     TimerId timer_id,
                                     Operation operation);

  void RunSync(const Operation& operation);

  bool ContainsDelayedOperation(TimerId timer_id) const;
  void RunDelayedOperationsUntil(TimerId last_timer_id);

  dispatch_queue_t dispatch_queue() const {
    return dispatch_queue_;
  }

 private:
  void Dispatch(const Operation& operation);

  void TryRemoveDelayedOperation(const DelayedOperationImpl& operation);

  bool OnTargetQueue() const;
  void VerifyOnTargetQueue() const;
  // GetLabel functions are guaranteed to never return a "null" string_view
  // (i.e. data() != nullptr).
  absl::string_view GetCurrentQueueLabel() const;
  absl::string_view GetTargetQueueLabel() const;

  std::atomic<dispatch_queue_t> dispatch_queue_;
  using DelayedOperationPtr = std::shared_ptr<DelayedOperationImpl>;
  std::vector<DelayedOperationPtr> operations_;
  std::atomic<bool> is_operation_in_progress_{false};

  // For access to TryRemoveDelayedOperation.
  friend class DelayedOperationImpl;
};

class DelayedOperationImpl
    : public std::enable_shared_from_this<DelayedOperationImpl> {
 public:
  DelayedOperationImpl(const std::shared_ptr<AsyncQueueImpl>& queue,
                       const TimerId timer_id,
                       const AsyncQueue::Milliseconds delay,
                       AsyncQueue::Operation&& operation)
      : queue_handle_{queue},
        timer_id_{timer_id},
        target_time_{std::chrono::time_point_cast<AsyncQueue::Milliseconds>(
                         std::chrono::system_clock::now()) +
                     delay},
        operation_{std::move(operation)} {
  }

  // Important: don't call `Start` from the constructor, `shared_from_this`
  // won't work.
  void Start(const dispatch_queue_t dispatch_queue,
             const AsyncQueue::Milliseconds delay) {
    namespace chr = std::chrono;
    const dispatch_time_t delay_ns = dispatch_time(
        DISPATCH_TIME_NOW, chr::duration_cast<chr::nanoseconds>(delay).count());
    // libdispatch must get its own shared pointer to the operation, otherwise
    // the operation might get destroyed by the time it's invoked (e.g., because
    // it was canceled or force-run; libdispatch will still get to run it, even
    // if it will be a no-op).
    const auto self = new StrongSelf(shared_from_this());
    dispatch_after_f(delay_ns, dispatch_queue, self,
                     DelayedOperationImpl::InvokedByLibdispatch);
  }

  void Cancel() {
    if (QueuePtr queue = queue_handle_.lock()) {
      TryDequeue(queue.get());
    }
    done_ = true;
  }

  void SkipDelay() {
    if (QueuePtr queue = queue_handle_.lock()) {
      queue->EnqueueAllowingSameQueue([this] { HandleDelayElapsed(); });
    }
  }

  TimerId timer_id() const {
    return timer_id_;
  }

  bool operator<(const DelayedOperationImpl& rhs) const {
    return target_time_ < rhs.target_time_;
  }

 private:
  static void InvokedByLibdispatch(void* const raw_self) {
    auto self = static_cast<StrongSelf*>(raw_self);
    if (QueuePtr queue = (*self)->queue_handle_.lock()) {
      queue->StartExecution([self] { (*self)->HandleDelayElapsed(); });
    }
    delete self;
  }

  void HandleDelayElapsed() {
    if (QueuePtr queue = queue_handle_.lock()) {
      TryDequeue(queue.get());

      if (!done_) {
        done_ = true;
        FIREBASE_ASSERT_MESSAGE(
            operation_,
            "DelayedOperationImpl contains invalid function object");
        operation_();
      }
    }
  }

  void TryDequeue(AsyncQueueImpl* const queue) {
    queue->VerifyIsCurrentQueue();
    queue->TryRemoveDelayedOperation(*this);
  }

  using TimePoint = std::chrono::time_point<std::chrono::system_clock,
                                            AsyncQueue::Milliseconds>;

  Executor* const executor_;
  const TimerId timer_id_;
  const TimePoint target_time_;
  const AsyncQueue::Operation operation_;

  // True if the operation has either been run or canceled.
  //
  // Note on thread-safety: `done_` is only ever accessed from `Cancel` and
  // `HandleDelayElapsed` member functions, both of which assert they are being
  // called while on the dispatch queue. In other words, `done_` is only
  // accessed when invoked by dispatch_async/dispatch_sync, both of which
  // provide synchronization.
  bool done_ = false;
};

}  // namespace internal

void DelayedOperation::Cancel() {
  queue_.TryCancel(*this);
}

// SerialQueue

void AsyncQueueImpl::TryRemoveDelayedOperation(
    const DelayedOperation& dequeued) {
  const auto found = std::find_if(operations_.begin(), operations_.end(),
                                  [&dequeued](const DelayedOperationPtr& op) {
                                    return op.get() == &dequeued;
                                  });
  // It's possible for the operation to be missing if libdispatch gets to run it
  // after it was force-run, for example.
  if (found != operations_.end()) {
    operations_.erase(found);
  }
}

// operations_.push_back(std::make_shared<DelayedOperationImpl>(
//     shared_from_this(), timer_id, delay, std::move(operation)));
// operations_.back()->Start(dispatch_queue(), delay);
// return DelayedOperation{operations_.back()};

bool AsyncQueueImpl::ContainsDelayedOperation(const TimerId timer_id) const {
  VerifyOnTargetQueue();
  return std::find_if(operations_.begin(), operations_.end(),
                      [timer_id](const DelayedOperationPtr& op) {
                        return op->timer_id() == timer_id;
                      }) != operations_.end();
}

// Private

bool AsyncQueueImpl::OnTargetQueue() const {
  return GetCurrentQueueLabel() == GetTargetQueueLabel();
}

void AsyncQueueImpl::VerifyOnTargetQueue() const {
  FIREBASE_ASSERT_MESSAGE(OnTargetQueue(),
                          "We are running on the wrong dispatch queue. "
                          "Expected '%s' Actual: '%s'",
                          GetTargetQueueLabel().data(),
                          GetCurrentQueueLabel().data());
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

namespace {

absl::string_view StringViewFromLabel(const char* const label) {
  // Make sure string_view's data is not null, because it's used for logging.
  return label ? absl::string_view{label} : absl::string_view{""};
}

}  // namespace

absl::string_view AsyncQueueImpl::GetCurrentQueueLabel() const {
  // Note: dispatch_queue_get_label may return nullptr if the queue wasn't
  // initialized with a label.
  return StringViewFromLabel(
      dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL));
}

absl::string_view AsyncQueueImpl::GetTargetQueueLabel() const {
  return StringViewFromLabel(dispatch_queue_get_label(dispatch_queue()));
}

namespace internal {

class DelayedOperationImpl {
 public:
  DelayedOperationImpl(const std::shared_ptr<AsyncQueueImpl>& queue,
                       const TimerId timer_id,
                       const AsyncQueue::Milliseconds delay,
                       AsyncQueue::Operation&& operation)
      : queue_handle_{queue},
        timer_id_{timer_id},
        target_time_{std::chrono::time_point_cast<AsyncQueue::Milliseconds>(
                         std::chrono::system_clock::now()) +
                     delay},
        operation_{std::move(operation)} {
  }

  // Important: don't call `Start` from the constructor, `shared_from_this`
  // won't work.
  void Start(const dispatch_queue_t dispatch_queue,
             const AsyncQueue::Milliseconds delay) {
    namespace chr = std::chrono;
    const dispatch_time_t delay_ns = dispatch_time(
        DISPATCH_TIME_NOW, chr::duration_cast<chr::nanoseconds>(delay).count());
    // libdispatch must get its own shared pointer to the operation, otherwise
    // the operation might get destroyed by the time it's invoked (e.g., because
    // it was canceled or force-run; libdispatch will still get to run it, even
    // if it will be a no-op).
    const auto self = new StrongSelf(shared_from_this());
    dispatch_after_f(delay_ns, dispatch_queue, self,
                     DelayedOperationImpl::InvokedByLibdispatch);
  }

  void Cancel() {
    if (QueuePtr queue = queue_handle_.lock()) {
      TryDequeue(queue.get());
    }
    done_ = true;
  }

  void SkipDelay() {
    if (QueuePtr queue = queue_handle_.lock()) {
      queue->EnqueueAllowingSameQueue([this] { HandleDelayElapsed(); });
    }
  }

  TimerId timer_id() const {
    return timer_id_;
  }

  bool operator<(const DelayedOperationImpl& rhs) const {
    return target_time_ < rhs.target_time_;
  }

 private:
  using StrongSelf = std::shared_ptr<DelayedOperationImpl>;
  using QueuePtr = std::shared_ptr<AsyncQueueImpl>;

  static void InvokedByLibdispatch(void* const raw_self) {
    auto self = static_cast<StrongSelf*>(raw_self);
    if (QueuePtr queue = (*self)->queue_handle_.lock()) {
      queue->EnterCheckedOperation([self] { (*self)->HandleDelayElapsed(); });
    }
    delete self;
  }

  void HandleDelayElapsed() {
    if (QueuePtr queue = queue_handle_.lock()) {
      TryDequeue(queue.get());

      if (!done_) {
        done_ = true;
        FIREBASE_ASSERT_MESSAGE(
            operation_,
            "DelayedOperationImpl contains invalid function object");
        operation_();
      }
    }
  }

  void TryDequeue(AsyncQueueImpl* const queue) {
    queue->VerifyIsCurrentQueue();
    queue->TryRemoveDelayedOperation(*this);
  }

  using TimePoint = std::chrono::time_point<std::chrono::system_clock,
                                            AsyncQueue::Milliseconds>;

  std::weak_ptr<AsyncQueueImpl> queue_handle_;
  const TimerId timer_id_;
  const TimePoint target_time_;
  const AsyncQueue::Operation operation_;

  // True if the operation has either been run or canceled.
  //
  // Note on thread-safety: `done_` is only ever accessed from `Cancel` and
  // `HandleDelayElapsed` member functions, both of which assert they are being
  // called while on the dispatch queue. In other words, `done_` is only
  // accessed when invoked by dispatch_async/dispatch_sync, both of which
  // provide synchronization.
  bool done_ = false;
};

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

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

  DelayedOperation ScheduleExecution(Milliseconds delay,
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
    return DelayedOperation{this, delayed_operation};
  }

 private:
  dispatch_queue_t dispatch_queue() const {
    return dispatch_queue_;
  }

  void TryRemoveDelayedOperation(const DelayedOperationImpl& operation);

  // GetLabel functions are guaranteed to never return a "null" string_view
  // (i.e. data() != nullptr).
  absl::string_view GetCurrentQueueLabel() const;
  absl::string_view GetTargetQueueLabel() const;

  std::atomic<dispatch_queue_t> dispatch_queue_;
  struct Op {
    Tag tag;
    DelayedOperationImpl* op_;
  };
  std::vector<Op> operations_;

  // For access to TryRemoveDelayedOperation.
  friend class DelayedOperationImpl;
};

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

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
    executor_->TryRemoveDelayedOperation(*this);
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

}  // namespace internal

}  // namespace util
}  // namespace firestore
}  // namespace firebase
