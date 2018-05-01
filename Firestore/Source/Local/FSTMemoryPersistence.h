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

#import "Firestore/Source/Local/FSTPersistence.h"
#include "Firestore/core/src/firebase/firestore/model/resource_path.h"

@protocol FSTLRUDelegate;
@class FSTLRUGarbageCollector;
@class FSTMaybeDocument;
@class FSTObjectValue;
@class FSTReferenceSet;

NS_ASSUME_NONNULL_BEGIN

/**
 * An in-memory implementation of the FSTPersistence protocol. Values are stored only in RAM and
 * are never persisted to any durable storage.
 */
@interface FSTMemoryPersistence : NSObject <FSTPersistence>

+ (instancetype)persistenceWithEagerGC;

+ (instancetype)persistenceWithLRUGC;

+ (instancetype)persistenceWithNoGC;

+ (size_t)objectValueSizeInMemory:(FSTObjectValue *)object;

+ (size_t)docSizeInMemory:(FSTMaybeDocument *)doc;

// This size calculation is specific to estimating in-memory size of paths.
// It should not be used for e.g. index entry sizing.
+ (size_t)pathSizeInMemory:(const firebase::firestore::model::ResourcePath &)path;

@end

@interface FSTMemoryLRUReferenceDelegate : NSObject <FSTReferenceDelegate, FSTLRUDelegate>

- (instancetype)initWithPersistence:(FSTMemoryPersistence *)persistence;

- (BOOL)isPinnedAtSequenceNumber:(FSTListenSequenceNumber)upperBound document:(FSTDocumentKey *)key;

@end

@interface FSTMemoryEagerReferenceDelegate : NSObject <FSTReferenceDelegate, FSTTransactional>

- (instancetype)initWithPersistence:(FSTMemoryPersistence *)persistence;

@end

NS_ASSUME_NONNULL_END
