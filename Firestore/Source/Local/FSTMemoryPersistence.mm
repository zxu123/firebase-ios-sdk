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

#import "Firestore/Source/Local/FSTMemoryPersistence.h"

#include <unordered_map>

#import "Firestore/Source/Local/FSTMemoryMutationQueue.h"
#import "Firestore/Source/Local/FSTMemoryQueryCache.h"
#import "Firestore/Source/Local/FSTMemoryRemoteDocumentCache.h"
#import "Firestore/Source/Local/FSTWriteGroup.h"
#import "Firestore/Source/Local/FSTWriteGroupTracker.h"
#import "Firestore/Source/Util/FSTAssert.h"

#include "Firestore/core/src/firebase/firestore/auth/user.h"
#include "Firestore/core/src/firebase/firestore/model/database_id.h"
#include "Firestore/core/src/firebase/firestore/model/resource_path.h"
#import "FSTDocument.h"
#import "FSTFieldValue.h"

using firebase::firestore::auth::HashUser;
using firebase::firestore::auth::User;
using firebase::firestore::model::DatabaseId;
using firebase::firestore::model::ResourcePath;

NS_ASSUME_NONNULL_BEGIN

@interface FSTMemoryPersistence ()
@property(nonatomic, strong, nonnull) FSTWriteGroupTracker *writeGroupTracker;
@property(nonatomic, assign, getter=isStarted) BOOL started;
@end

@implementation FSTMemoryPersistence {
  /**
   * The FSTQueryCache representing the persisted cache of queries.
   *
   * Note that this is retained here to make it easier to write tests affecting both the in-memory
   * and LevelDB-backed persistence layers. Tests can create a new FSTLocalStore wrapping this
   * FSTPersistence instance and this will make the in-memory persistence layer behave as if it
   * were actually persisting values.
   */
  FSTMemoryQueryCache *_queryCache;

  /** The FSTRemoteDocumentCache representing the persisted cache of remote documents. */
  FSTMemoryRemoteDocumentCache *_remoteDocumentCache;

  std::unordered_map<User, FSTMemoryMutationQueue*, HashUser> _mutationQueues;
}

+ (instancetype)persistence {
  return [[FSTMemoryPersistence alloc] init];
}

+ (size_t)valueSizeInMemory:(FSTFieldValue *)fieldValue {
  Class fieldClass = [fieldValue class];
  if (fieldClass == [FSTNullValue class]) {
    return 0;
  } else if (fieldClass == [FSTBooleanValue class]) {
    return sizeof(bool);
  } else if (fieldClass == [FSTIntegerValue class]) {
    return sizeof(int64_t);
  } else if (fieldClass == [FSTDoubleValue class]) {
    return sizeof(double);
  } else if (fieldClass == [FSTStringValue class]) {
    return [fieldValue.value length];
  } else if (fieldClass == [FSTTimestampValue class]) {
    return sizeof(int64_t) + sizeof(int32_t);
  } else if (fieldClass == [FSTGeoPointValue class]) {
    return 2 * sizeof(double);
  } else if (fieldClass == [FSTBlobValue class]) {
    return ((NSData *)fieldValue.value).length;
  } else if (fieldClass == [FSTReferenceValue class]) {
    return sizeof(DatabaseId) + [FSTMemoryPersistence pathSizeInMemory:((FSTDocumentKey *)fieldValue.value).path];
  } else if (fieldClass == [FSTObjectValue class]) {
    return [FSTMemoryPersistence objectValueSizeInMemory:(FSTObjectValue *)fieldValue];
  } else if (fieldClass == [FSTArrayValue class]) {
    size_t result = 0;
    NSArray<FSTFieldValue *> *elems = (NSArray<FSTFieldValue *> *)fieldValue.value;
    for (FSTFieldValue *elem in elems) {
      result += [FSTMemoryPersistence valueSizeInMemory:elem];
    }
    return result;
  }
  FSTFail(@"Unknown FieldValue type: %@", fieldClass);
}

+ (size_t)objectValueSizeInMemory:(FSTObjectValue *)object {
  __block size_t result = 0;
  [object.internalValue enumerateKeysAndObjectsUsingBlock:^(NSString *key, FSTFieldValue *value, BOOL *stop) {
    result += key.length;
    result += [FSTMemoryPersistence valueSizeInMemory:value];
  }];
  return result;
}

+ (size_t)docSizeInMemory:(FSTMaybeDocument *)doc {
  size_t result = [FSTMemoryPersistence pathSizeInMemory:doc.key.path()];
  if ([doc isKindOfClass:[FSTDocument class]]) {
    FSTObjectValue *value = ((FSTDocument *)doc).data;
    result += [FSTMemoryPersistence objectValueSizeInMemory:value];
  }
  return result;
}

+ (size_t)pathSizeInMemory:(const ResourcePath &)path {
  size_t result = 0;
  for (auto it = path.begin(); it != path.end(); it++) {
    result += it->size();
  }
  return result;
}

- (instancetype)init {
  if (self = [super init]) {
    _writeGroupTracker = [FSTWriteGroupTracker tracker];
    _queryCache = [[FSTMemoryQueryCache alloc] init];
    _remoteDocumentCache = [[FSTMemoryRemoteDocumentCache alloc] init];
  }
  return self;
}

- (BOOL)start:(NSError **)error {
  // No durable state to read on startup.
  FSTAssert(!self.isStarted, @"FSTMemoryPersistence double-started!");
  self.started = YES;
  return YES;
}

- (void)shutdown {
  // No durable state to ensure is closed on shutdown.
  FSTAssert(self.isStarted, @"FSTMemoryPersistence shutdown without start!");
  self.started = NO;
}

- (id<FSTMutationQueue>)mutationQueueForUser:(const User &)user {
  id<FSTMutationQueue> queue = _mutationQueues[user];
  if (!queue) {
    queue = [FSTMemoryMutationQueue mutationQueue];
    _mutationQueues[user] = queue;
  }
  return queue;
}

- (id<FSTQueryCache>)queryCache {
  return _queryCache;
}

- (id<FSTRemoteDocumentCache>)remoteDocumentCache {
  return _remoteDocumentCache;
}

- (FSTWriteGroup *)startGroupWithAction:(NSString *)action {
  return [self.writeGroupTracker startGroupWithAction:action];
}

- (void)commitGroup:(FSTWriteGroup *)group {
  [self.writeGroupTracker endGroup:group];

  FSTAssert(group.isEmpty, @"Memory persistence shouldn't use write groups: %@", group.action);
}

- (long)byteSize {
  long bytes = [_queryCache byteSize] + [_remoteDocumentCache byteSize];
  for (auto it = _mutationQueues.begin(); it != _mutationQueues.end(); ++it) {
    bytes += [it->second byteSize];
  }
  return bytes;
}

@end

NS_ASSUME_NONNULL_END
