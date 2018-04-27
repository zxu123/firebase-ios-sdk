/*
 * Copyright 2017 Google
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

#import <Foundation/Foundation.h>

#include <memory>
#include <utility>

#import "Firestore/Source/Util/FSTAssert.h"
#import "Firestore/Source/Util/FSTDispatchQueue.h"
#include "Firestore/core/src/firebase/firestore/util/async_queue.h"
#include "Firestore/core/src/firebase/firestore/util/executor.h"
#include "Firestore/core/src/firebase/firestore/util/executor_libdispatch.h"
#include "absl/memory/memory.h"

#include <type_traits>

using firebase::firestore::util::AsyncQueue;
using firebase::firestore::util::DelayedOperation;
using firebase::firestore::util::TimerId;
using firebase::firestore::util::internal::Executor;
using firebase::firestore::util::internal::ExecutorLibdispatch;

NS_ASSUME_NONNULL_BEGIN

#pragma mark - FSTDelayedCallback

@interface FSTDelayedCallback () {
  DelayedOperation _impl;
}

@end

@implementation FSTDelayedCallback

- (instancetype)initWithImpl:(DelayedOperation &&)impl {
  if (self = [super init]) {
    _impl = std::move(impl);
  }
  return self;
}

- (void)cancel {
  _impl.Cancel();
}

@end

#pragma mark - FSTDispatchQueue

@implementation FSTDispatchQueue {
  std::unique_ptr<AsyncQueue> _impl;
  ExecutorLibdispatch* _hack;
}

-(dispatch_queue_t) queue {
  return _hack->dispatch_queue();
}

+ (instancetype)queueWith:(dispatch_queue_t)dispatchQueue {
  return [[FSTDispatchQueue alloc] initWithQueue:dispatchQueue];
}

- (instancetype)initWithQueue:(dispatch_queue_t)queue {
  if (self = [super init]) {
    // std::unique_ptr<Executor> executor{new ExecutorLibdispatch(queue)};
    _hack = new ExecutorLibdispatch();
    std::unique_ptr<Executor> executor{_hack};
    _impl = absl::make_unique<AsyncQueue>(std::move(executor));
  }
  return self;
}

- (void)verifyIsCurrentQueue {
  _impl->VerifyCalledFromOperation();
}

- (void)enterCheckedOperation:(void (^)(void))block {
  _impl->StartExecution([block] { block(); });
}

- (void)dispatchAsync:(void (^)(void))block {
  _impl->Enqueue([block] { block(); });
}

- (void)dispatchAsyncAllowingSameQueue:(void (^)(void))block {
  _impl->EnqueueAllowingNesting([block] { block(); });
}

- (void)dispatchSync:(void (^)(void))block {
  _impl->EnqueueBlocking([block] { block(); });
}

- (FSTDelayedCallback *)dispatchAfterDelay:(NSTimeInterval)delay
                                   timerID:(FSTTimerID)timerID
                                     block:(void (^)(void))block {
  const AsyncQueue::Milliseconds delay_ms = std::chrono::milliseconds(static_cast<long long>(delay * 1000));
  const TimerId timer_id = static_cast<TimerId>(timerID);
  DelayedOperation delayed_operation =
      _impl->EnqueueAfterDelay(delay_ms, timer_id, [block] { block(); });
  return [[FSTDelayedCallback alloc] initWithImpl:std::move(delayed_operation)];
  // return nil;
}

- (BOOL)containsDelayedCallbackWithTimerID:(FSTTimerID)timerID {
  return _impl->IsScheduled(static_cast<TimerId>(timerID));
  // return NO;
}

- (void)runDelayedCallbacksUntil:(FSTTimerID)lastTimerID {
  _impl->RunScheduledOperationsUntil(static_cast<TimerId>(lastTimerID));
}

@end

NS_ASSUME_NONNULL_END
