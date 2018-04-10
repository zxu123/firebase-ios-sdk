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

#import "Firestore/Source/Local/FSTMemoryQueryCache.h"

#import "FSTMemoryPersistence.h"
#import "Firestore/Source/Core/FSTQuery.h"
#import "Firestore/Source/Core/FSTSnapshotVersion.h"
#import "Firestore/Source/Local/FSTQueryData.h"
#import "Firestore/Source/Local/FSTReferenceSet.h"
#import "Firestore/Source/Model/FSTDocumentKey.h"
#import "Firestore/Source/Remote/FSTRemoteEvent.h"

#include "Firestore/core/src/firebase/firestore/model/document_key.h"
#import "FSTDocumentReference.h"

using firebase::firestore::model::DocumentKey;

NS_ASSUME_NONNULL_BEGIN

@interface FSTMemoryQueryCache ()

/** Maps a query to the data about that query. */
@property(nonatomic, strong, readonly) NSMutableDictionary<FSTQuery *, FSTQueryData *> *queries;

@property(nonatomic, strong, readonly)
    NSMutableDictionary<FSTDocumentKey *, NSNumber *> *orphanedDocumentSequenceNumbers;

/** A ordered bidirectional mapping between documents and the remote target IDs. */
@property(nonatomic, strong, readonly) FSTReferenceSet *references;

/** The highest numbered target ID encountered. */
@property(nonatomic, assign) FSTTargetID highestTargetID;

@property(nonatomic, assign) FSTListenSequenceNumber highestListenSequenceNumber;

/** The last received snapshot version. */
@property(nonatomic, strong) FSTSnapshotVersion *lastRemoteSnapshotVersion;

@end

@implementation FSTMemoryQueryCache {
  FSTMemoryPersistence *_persistence;
}

- (instancetype)initWithPersistence:(FSTMemoryPersistence *)persistence {
  if (self = [super init]) {
    _persistence = persistence;
    _queries = [NSMutableDictionary dictionary];
    _orphanedDocumentSequenceNumbers = [NSMutableDictionary dictionary];
    _references = [[FSTReferenceSet alloc] init];
    _lastRemoteSnapshotVersion = [FSTSnapshotVersion noVersion];
  }
  return self;
}

#pragma mark - FSTQueryCache implementation
#pragma mark Query tracking

- (void)start {
  // Nothing to do.
}

- (FSTTargetID)highestTargetID {
  return _highestTargetID;
}

- (FSTListenSequenceNumber)highestListenSequenceNumber {
  return _highestListenSequenceNumber;
}

- (void)addQueryData:(FSTQueryData *)queryData {
  self.queries[queryData.query] = queryData;
  if (queryData.targetID > self.highestTargetID) {
    self.highestTargetID = queryData.targetID;
  }
  if (queryData.sequenceNumber > self.highestListenSequenceNumber) {
    self.highestListenSequenceNumber = queryData.sequenceNumber;
  }
}

- (void)updateQueryData:(FSTQueryData *)queryData {
  self.queries[queryData.query] = queryData;
  if (queryData.targetID > self.highestTargetID) {
    self.highestTargetID = queryData.targetID;
  }
  if (queryData.sequenceNumber > self.highestListenSequenceNumber) {
    self.highestListenSequenceNumber = queryData.sequenceNumber;
  }
}

- (int32_t)count {
  return (int32_t)[self.queries count];
}

- (void)removeQueryData:(FSTQueryData *)queryData sequenceNumber:(FSTListenSequenceNumber)sequenceNumber {
  [self.queries removeObjectForKey:queryData.query];
  [self.references enumerateReferencesForID:queryData.targetID block:^(FSTDocumentReference *ref, BOOL *stop) {
    [self->_persistence.referenceDelegate removeReference:ref.key
                                                   target:queryData.targetID
                                           sequenceNumber:sequenceNumber];
  }];
  [self.references removeReferencesForID:queryData.targetID];
}

- (nullable FSTQueryData *)queryDataForQuery:(FSTQuery *)query {
  return self.queries[query];
}

- (void)enumerateTargetsUsingBlock:(void (^)(FSTQueryData *queryData, BOOL *stop))block {
  [self.queries
      enumerateKeysAndObjectsUsingBlock:^(FSTQuery *key, FSTQueryData *queryData, BOOL *stop) {
        block(queryData, stop);
      }];
}

- (void)enumerateOrphanedDocumentsUsingBlock:
    (void (^)(FSTDocumentKey *docKey, FSTListenSequenceNumber sequenceNumber, BOOL *stop))block {
  [self.orphanedDocumentSequenceNumbers
      enumerateKeysAndObjectsUsingBlock:^(FSTDocumentKey *key, NSNumber *sequenceNumber,
                                          BOOL *stop) {
        block(key, [sequenceNumber longLongValue], stop);
      }];
}

- (NSUInteger)removeQueriesThroughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber
                                     liveQueries:
                                         (NSDictionary<NSNumber *, FSTQueryData *> *)liveQueries {
  NSMutableArray<FSTQuery *> *toRemove = [NSMutableArray array];
  [self.queries
      enumerateKeysAndObjectsUsingBlock:^(FSTQuery *query, FSTQueryData *queryData, BOOL *stop) {
        if (queryData.sequenceNumber <= sequenceNumber) {
          if (liveQueries[@(queryData.targetID)] == nil) {
            [toRemove addObject:query];
            [self.references removeReferencesForID:queryData.targetID];
          }
        }
      }];
  [self.queries removeObjectsForKeys:toRemove];
  return [toRemove count];
}

#pragma mark Reference tracking

- (void)addPotentiallyOrphanedDocuments:(FSTDocumentKeySet *)keys
                       atSequenceNumber:(FSTListenSequenceNumber)sequenceNumber {
  NSNumber *seqNum = @(sequenceNumber);
  [keys enumerateObjectsUsingBlock:^(FSTDocumentKey *key, BOOL *stop) {
    self.orphanedDocumentSequenceNumbers[key] = seqNum;
  }];
}

- (void)addMatchingKeys:(FSTDocumentKeySet *)keys
            forTargetID:(FSTTargetID)targetID
       atSequenceNumber:(FSTListenSequenceNumber)sequenceNumber {
  // We're adding docs to a target, we no longer care that they were mutated.
  for (FSTDocumentKey *key in [keys objectEnumerator]) {
    [_persistence.referenceDelegate addReference:key
                                          target:targetID
                                  sequenceNumber:sequenceNumber];
  }
  [self.references addReferencesToKeys:keys forID:targetID];
}

- (void)removeMatchingKeys:(FSTDocumentKeySet *)keys
               forTargetID:(FSTTargetID)targetID
          atSequenceNumber:(FSTListenSequenceNumber)sequenceNumber {
  [self.references removeReferencesToKeys:keys forID:targetID];
  [keys enumerateObjectsUsingBlock:^(FSTDocumentKey *key, BOOL *stop) {
    [_persistence.referenceDelegate removeReference:key
                                             target:targetID
                                     sequenceNumber:sequenceNumber];
  }];
}

- (void)removeMatchingKeysForTargetID:(FSTTargetID)targetID {

  [self.references removeReferencesForID:targetID];
}



- (FSTDocumentKeySet *)matchingKeysForTargetID:(FSTTargetID)targetID {
  return [self.references referencedKeysForID:targetID];
}

- (FSTRemovalResult)removeOrphanedDocument:(FSTDocumentKey *)key
                    upperBound:(FSTListenSequenceNumber)upperBound {
  NSNumber *seq = self.orphanedDocumentSequenceNumbers[key];
  if (!seq) {
    return FSTDocumentNonexistent;
  } else if ([seq longLongValue] <= upperBound) {
    [self.orphanedDocumentSequenceNumbers removeObjectForKey:key];
    return FSTDocumentRemoved;
  } else {
    return FSTDocumentRetained;
  }
}

- (nullable FSTQueryData *)handleTargetChange:(FSTTargetChange *)change
                                    queryData:(FSTQueryData *)queryData
                                     orphaned:(std::set<FSTDocumentKey *> &)orphaned {
  FSTTargetMapping *mapping = change.mapping;
  if (mapping) {
    // First make sure that all references are deleted.
    if ([mapping isKindOfClass:[FSTResetMapping class]]) {
      FSTResetMapping *reset = (FSTResetMapping *)mapping;
      [self.references enumerateReferencesForID:queryData.targetID block:^(FSTDocumentReference *ref, BOOL *stop) {
        [self.references removeReference:ref];
        FSTDocumentKey* key = ref.key;
        [_persistence.referenceDelegate removeReference:key
                                                 target:queryData.targetID
                                         sequenceNumber:queryData.sequenceNumber];
      }];

      for (FSTDocumentKey *key in [reset.documents objectEnumerator]) {
        [self.orphanedDocumentSequenceNumbers removeObjectForKey:key];
        [_persistence.referenceDelegate addReference:key
                                              target:queryData.targetID
                                      sequenceNumber:queryData.sequenceNumber];
        [self.references addReferenceToKey:key forID:queryData.targetID];
      }
    } else if ([mapping isKindOfClass:[FSTUpdateMapping class]]) {
      FSTUpdateMapping *update = (FSTUpdateMapping *)mapping;
      for (FSTDocumentKey *key in [update.removedDocuments objectEnumerator]) {
        [self.references removeReferenceToKey:key forID:queryData.targetID];
        [_persistence.referenceDelegate removeReference:key
                                                 target:queryData.targetID
                                         sequenceNumber:queryData.sequenceNumber];
      }

      for (FSTDocumentKey *key in [update.addedDocuments objectEnumerator]) {
        [self.orphanedDocumentSequenceNumbers removeObjectForKey:key];
        [_persistence.referenceDelegate addReference:key
                                              target:queryData.targetID
                                      sequenceNumber:queryData.sequenceNumber];
        [self.references addReferenceToKey:key forID:queryData.targetID];
      }
    } else {
      FSTFail(@"Unknown mapping type: %@", mapping);
    }
  }
  return queryData;
}

#pragma mark - Sizing

- (long)byteSize {
  __block long result = 0;
  [self.orphanedDocumentSequenceNumbers
      enumerateKeysAndObjectsUsingBlock:^(FSTDocumentKey *key, NSNumber *obj, BOOL *stop) {
        result += [FSTMemoryPersistence pathSizeInMemory:key.path];
        result += sizeof(int64_t);  // account for the number
      }];
  [self.queries
      enumerateKeysAndObjectsUsingBlock:^(FSTQuery *query, FSTQueryData *queryData, BOOL *stop) {
        // The queryData also includes the query, so we can use that calculation twice.
        result += 2 * query.canonicalID.length;
        result += queryData.resumeToken.length;
        // Technically we are ignoring a small amount of QueryData overhead, we are just
        // capturing the dynamic elements.
      }];
  return result;
}

#pragma mark - FSTGarbageSource implementation

- (nullable id<FSTGarbageCollector>)garbageCollector {
  return self.references.garbageCollector;
}

- (void)setGarbageCollector:(nullable id<FSTGarbageCollector>)garbageCollector {
  //self.references.garbageCollector = garbageCollector;
}

- (BOOL)containsKey:(const firebase::firestore::model::DocumentKey &)key {
  // We intentionally ignore orphaned documents here, they are not part of a query and so
  // are not 'contained' by the query cache.
  return [self.references containsKey:key];
}

@end

NS_ASSUME_NONNULL_END
