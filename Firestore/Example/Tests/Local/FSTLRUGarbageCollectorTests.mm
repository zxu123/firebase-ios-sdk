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

#import "Firestore/Example/Tests/Local/FSTLRUGarbageCollectorTests.h"

#import <XCTest/XCTest.h>
#import <absl/strings/str_cat.h>

#import "Firestore/Example/Tests/Util/FSTHelpers.h"
#import "Firestore/Source/Local/FSTLRUGarbageCollector.h"
#import "Firestore/Source/Local/FSTMutationQueue.h"
#import "Firestore/Source/Local/FSTPersistence.h"
#import "Firestore/Source/Local/FSTQueryCache.h"
#import "Firestore/Source/Model/FSTDocument.h"
#import "Firestore/Source/Model/FSTFieldValue.h"
#import "Firestore/Source/Model/FSTMutation.h"
#import "Firestore/Source/Util/FSTClasses.h"
#include "Firestore/core/src/firebase/firestore/auth/user.h"
#include "Firestore/core/src/firebase/firestore/model/document_key.h"
#include "Firestore/core/src/firebase/firestore/model/document_key_set.h"
#include "Firestore/core/src/firebase/firestore/model/precondition.h"
#include "Firestore/core/test/firebase/firestore/testutil/testutil.h"

using firebase::firestore::auth::User;
using firebase::firestore::model::DocumentKey;
using firebase::firestore::model::DocumentKeySet;
using firebase::firestore::model::Precondition;
namespace testutil = firebase::firestore::testutil;

NS_ASSUME_NONNULL_BEGIN

@implementation FSTLRUGarbageCollectorTests {
  FSTTargetID _previousTargetID;
  NSUInteger _previousDocNum;
  FSTObjectValue *_testValue;
  FSTObjectValue *_bigObjectValue;
}

- (id<FSTPersistence>)newPersistence {
  @throw FSTAbstractMethodException();  // NOLINT
}

- (long)compactedSize:(id<FSTPersistence>)persistence {
  @throw FSTAbstractMethodException();  // NOLINT
}

- (FSTLRUGarbageCollector *)gcForPersistence:(id<FSTPersistence>)persistence {
  id<FSTLRUDelegate> delegate = (id<FSTLRUDelegate>)persistence.referenceDelegate;
  return delegate.gc;
}

- (void)setUp {
  [super setUp];

  _previousTargetID = 500;
  _previousDocNum = 10;
  _testValue = FSTTestObjectValue(@{ @"baz" : @YES, @"ok" : @"fine" });
  NSString *bigString = [@"" stringByPaddingToLength:4096 withString:@"a" startingAtIndex:0];
  _bigObjectValue = FSTTestObjectValue(@{
          @"BigProperty": bigString
  });
}

- (BOOL)isTestBaseClass {
  return ([self class] == [FSTLRUGarbageCollectorTests class]);
}

- (FSTQueryData *)nextTestQuery:(id<FSTPersistence>)persistence {
  FSTTargetID targetID = ++_previousTargetID;
  FSTListenSequenceNumber listenSequenceNumber = persistence.referenceDelegate.sequenceNumber;
  FSTQuery *query = FSTTestQuery(absl::StrCat("path", targetID));
  return [[FSTQueryData alloc] initWithQuery:query
                                    targetID:targetID
                        listenSequenceNumber:listenSequenceNumber
                                     purpose:FSTQueryPurposeListen];
}

- (FSTDocumentKey *)nextTestDocKey {
  NSString *path = [NSString stringWithFormat:@"docs/doc_%lu", (unsigned long)++_previousDocNum];
  return FSTTestDocKey(path);
}

- (FSTDocument *)nextTestDocumentWithValue:(FSTObjectValue *)value {
  FSTDocumentKey *key = [self nextTestDocKey];
  FSTTestSnapshotVersion version = 2;
  BOOL hasMutations = NO;
  return [FSTDocument documentWithData:value
                                   key:key
                               version:testutil::Version(version)
                     hasLocalMutations:hasMutations];
}

- (FSTDocument *)nextTestDocument {
  return [self nextTestDocumentWithValue:_testValue];
}

- (FSTDocument *)nextBigTestDocument {
  return [self nextTestDocumentWithValue:_bigObjectValue];
}

- (void)testPickSequenceNumberPercentile {
  if ([self isTestBaseClass]) return;

  const int numTestCases = 5;
  struct Case {
    // number of queries to cache
    int queries;
    // number expected to be calculated as 10%
    int expected;
  };
  struct Case testCases[numTestCases] = {{0, 0}, {10, 1}, {9, 0}, {50, 5}, {49, 4}};

  for (int i = 0; i < numTestCases; i++) {
    // Fill the query cache.
    int numQueries = testCases[i].queries;
    int expectedTenthPercentile = testCases[i].expected;
    id<FSTPersistence> persistence = [self newPersistence];
    id<FSTQueryCache> queryCache = [persistence queryCache];
    [queryCache start];
    for (int j = 0; j < numQueries; j++) {
      persistence.run("testPickSequenceNumberPercentile" + std::to_string(i) + ", " + std::to_string(j), [&]() {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      });
    }
    FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
    FSTListenSequenceNumber tenth = [gc queryCountForPercentile:10];
    XCTAssertEqual(expectedTenthPercentile, tenth, @"Total query count: %i", numQueries);
    [persistence shutdown];
  }
}

- (void)testSequenceNumberForQueryCount {
  if ([self isTestBaseClass]) return;

  // Sequence numbers in this test start at 1001 and are incremented by one.

  // No queries... should get invalid sequence number (-1)
  {
    id<FSTPersistence> persistence = [self newPersistence];
    persistence.run("no queries", [&]() {
      id<FSTQueryCache> queryCache = [persistence queryCache];
      [queryCache start];
      FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
      FSTListenSequenceNumber highestToCollect = [gc sequenceNumberForQueryCount:0];
      XCTAssertEqual(kFSTListenSequenceNumberInvalid, highestToCollect);
    });
    [persistence shutdown];
  }

  // 50 queries, want 10. Should get 1010.
  {
    id<FSTPersistence> persistence = [self newPersistence];
    id<FSTQueryCache> queryCache = [persistence queryCache];
    [queryCache start];
    FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
    for (int i = 0; i < 50; i++) {
      persistence.run("add query", [&]() {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      });
    }
    FSTListenSequenceNumber highestToCollect = persistence.run("find sequence number", [&]() -> FSTListenSequenceNumber {
      return [gc sequenceNumberForQueryCount:10];
    });
    XCTAssertEqual(10, highestToCollect);
    [persistence shutdown];
  }

  // 50 queries, 9 with 1, incrementing from there. Should get 2.
  {
    id<FSTPersistence> persistence = [self newPersistence];
    id<FSTQueryCache> queryCache = [persistence queryCache];
    [queryCache start];
    FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
    persistence.run("initial batch", [&]() {
      for (int i = 0; i < 9; i++) {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      }
    });

    for (int i = 9; i < 50; i++) {
      persistence.run("incrementing sequence number", [&]() {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      });
    }
    FSTListenSequenceNumber highestToCollect = persistence.run("find sequence number", [&]() -> FSTListenSequenceNumber {
      return [gc sequenceNumberForQueryCount:10];
    });
    XCTAssertEqual(2, highestToCollect);
    [persistence shutdown];
  }

  // 50 queries, 11 with 1, incrementing from there. Should get 1.
  {
    id<FSTPersistence> persistence = [self newPersistence];
    id<FSTQueryCache> queryCache = [persistence queryCache];
    [queryCache start];
    FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
    persistence.run("Initial batch", [&]() {
      for (int i = 0; i < 11; i++) {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      }
    });
    for (int i = 11; i < 50; i++) {
      persistence.run("incrementing sequence number", [&]() {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      });
    }
    FSTListenSequenceNumber highestToCollect = persistence.run("find sequence number", [&]() -> FSTListenSequenceNumber {
      return [gc sequenceNumberForQueryCount:10];
    });
    XCTAssertEqual(1, highestToCollect);
    [persistence shutdown];
  }

  // A mutated doc at 1, 50 queries 2-51. Should get 10.
  {
    id<FSTPersistence> persistence = [self newPersistence];
    id<FSTQueryCache> queryCache = [persistence queryCache];
    [queryCache start];
    FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
    FSTDocumentKey *key = [self nextTestDocKey];
    persistence.run("remove mutation", [&]() {
      [persistence.referenceDelegate removeMutationReference:key];
    });
    for (int i = 0; i < 50; i++) {
      persistence.run("incrementing sequence number", [&]() {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      });
    }
    FSTListenSequenceNumber highestToCollect = persistence.run("find sequence number", [&]() -> FSTListenSequenceNumber {
      return [gc sequenceNumberForQueryCount:10];
    });
    XCTAssertEqual(10, highestToCollect);
    [persistence shutdown];
  }

  // Add mutated docs, then add one of them to a query target so it doesn't get GC'd.
  // Expect 3 (8 docs @ 1, two sequential queries, 2, and 3).
  {
    id<FSTPersistence> persistence = [self newPersistence];
    id<FSTQueryCache> queryCache = [persistence queryCache];
    [queryCache start];
    FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
    FSTDocument *docInQuery = [self nextTestDocument];
    DocumentKeySet docInQuerySet{docInQuery.key};
    // Adding 9 doc keys at 1. If we remove one of them, we'll have room for two actual
    // queries.
    persistence.run("mutations at same sequence number: 1", [&]() {
      [persistence.referenceDelegate removeMutationReference:docInQuery.key];
      for (int i = 0; i < 8; i++) {
        [persistence.referenceDelegate removeMutationReference:[self nextTestDocKey]];
      }
    });

    for (int i = 0; i < 49; i++) {
      persistence.run("incrementing sequence number", [&]() {
        [queryCache addQueryData:[self nextTestQuery:persistence]];
      });
    }

    persistence.run("Add query", [&]() {
      FSTQueryData *queryData = [self nextTestQuery:persistence];
      [queryCache addQueryData:queryData];
      // This should bump one document out of the mutated documents cache.
      [queryCache addMatchingKeys:docInQuerySet
                      forTargetID:queryData.targetID];
    });

    // This should catch the remaining 8 documents, plus the first two queries we added.
    FSTListenSequenceNumber highestToCollect = persistence.run("find sequence number", [&]() -> FSTListenSequenceNumber {
      return [gc sequenceNumberForQueryCount:10];
    });
    XCTAssertEqual(3, highestToCollect);
    [persistence shutdown];
  }
}

- (void)testRemoveQueriesUpThroughSequenceNumber {
  if ([self isTestBaseClass]) return;

  id<FSTPersistence> persistence = [self newPersistence];
  id<FSTQueryCache> queryCache = [persistence queryCache];
  [queryCache start];
  FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
  NSMutableDictionary<NSNumber *, FSTQueryData *> *liveQueries =
      [[NSMutableDictionary alloc] init];
  for (int i = 0; i < 100; i++) {
    persistence.run("incrementing sequence numbers", [&]() {
      FSTQueryData *queryData = [self nextTestQuery:persistence];
      // Mark odd queries as live so we can test filtering out live queries.
      if (queryData.targetID % 2 == 1) {
        liveQueries[@(queryData.targetID)] = queryData;
      }
      [queryCache addQueryData:queryData];
    });
  }

  // GC up through 15, which is 15%.
  // Expect to have GC'd 7 targets (even values of 1-15).
  NSUInteger removed = persistence.run("do GC", [&]() -> NSUInteger {
    return [gc removeQueriesUpThroughSequenceNumber:15 liveQueries:liveQueries];
  });
  XCTAssertEqual(7, removed);
  [persistence shutdown];
}

- (void)testRemoveOrphanedDocuments {
  if ([self isTestBaseClass]) return;

  id<FSTPersistence> persistence = [self newPersistence];
  id<FSTQueryCache> queryCache = [persistence queryCache];
  id<FSTRemoteDocumentCache> documentCache = [persistence remoteDocumentCache];
  User user("user");
  FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
  id<FSTMutationQueue> mutationQueue = [persistence mutationQueueForUser:user];
  persistence.run("start", [&]() {
    [queryCache start];
    [mutationQueue start];
  });

  // Add docs to mutation queue, as well as keep some queries. verify that correct documents are
  // removed.
  NSMutableSet<FSTDocumentKey *> *toBeRetained = [NSMutableSet set];

  NSMutableArray *mutations = [NSMutableArray arrayWithCapacity:2];
  // Add two documents to first target, and register a mutation on the second one
  persistence.run("add docs and mutation", [&]() {
    FSTQueryData *queryData = [self nextTestQuery:persistence];
    [queryCache addQueryData:queryData];
    DocumentKeySet keySet{};
    FSTDocument *doc1 = [self nextTestDocument];
    [documentCache addEntry:doc1];
    keySet = keySet.insert(doc1.key);
    [toBeRetained addObject:doc1.key];
    FSTDocument *doc2 = [self nextTestDocument];
    [documentCache addEntry:doc2];
    keySet = keySet.insert(doc2.key);
    [toBeRetained addObject:doc2.key];
    [queryCache addMatchingKeys:keySet forTargetID:queryData.targetID];

    FSTObjectValue *newValue = [[FSTObjectValue alloc]
        initWithDictionary:@{@"foo" : [FSTStringValue stringValue:@"bar"]}];
    [mutations addObject:[[FSTSetMutation alloc] initWithKey:doc2.key
                                                       value:newValue
                                                precondition:Precondition::None()]];
  });

  // Add one document to the second target
  persistence.run("add document to second target", [&]() {
    FSTQueryData *queryData = [self nextTestQuery:persistence];
    [queryCache addQueryData:queryData];
    DocumentKeySet keySet{};
    FSTDocument *doc1 = [self nextTestDocument];
    [documentCache addEntry:doc1];
    keySet = keySet.insert(doc1.key);
    [toBeRetained addObject:doc1.key];
    [queryCache addMatchingKeys:keySet forTargetID:queryData.targetID];
  });

  persistence.run("Add new document", [&]() {
    FSTDocument *doc1 = [self nextTestDocument];
    [mutations addObject:[[FSTSetMutation alloc] initWithKey:doc1.key
                                                       value:doc1.data
                                                precondition:Precondition::None()]];
    [documentCache addEntry:doc1];
    [toBeRetained addObject:doc1.key];
  });

  persistence.run("Add mutations", [&]() {
    FIRTimestamp *writeTime = [FIRTimestamp timestamp];
    [mutationQueue addMutationBatchWithWriteTime:writeTime mutations:mutations];
  });

  // Now add the docs we expect to get resolved.
  NSUInteger expectedRemoveCount = 5;
  NSMutableSet<FSTDocumentKey *> *toBeRemoved =
          [NSMutableSet setWithCapacity:expectedRemoveCount];
  persistence.run("Add documents", [&]() {
    DocumentKeySet removedSet{};
    for (int i = 0; i < expectedRemoveCount; i++) {
      FSTDocument *doc = [self nextTestDocument];
      [toBeRemoved addObject:doc.key];
      [documentCache addEntry:doc];
      removedSet = removedSet.insert(doc.key);
      [persistence.referenceDelegate removeMutationReference:doc.key];
    }
  });

  //[queryCache addPotentiallyOrphanedDocuments:removedSet atSequenceNumber:1000];
  NSUInteger removed = persistence.run("GC orpahened", [&]() -> NSUInteger {
    return [gc removeOrphanedDocuments:documentCache
                 throughSequenceNumber:1000
                         mutationQueue:mutationQueue];
  });

  persistence.run("verification", [&]() {
    XCTAssertEqual(expectedRemoveCount, removed);
    for (FSTDocumentKey *key in toBeRemoved) {
      XCTAssertNil([documentCache entryForKey:key]);
      XCTAssertFalse([queryCache containsKey:key]);
    }
    for (FSTDocumentKey *key in toBeRetained) {
      XCTAssertNotNil([documentCache entryForKey:key], @"Missing document %@", key);
    }
  });

  [persistence shutdown];
}

// TODO(gsoltis): write a test that includes limbo documents

- (void)testRemoveTargetsThenGC {
  if ([self isTestBaseClass]) return;

  // Create 3 targets, add docs to all of them
  // Leave oldest target alone, it is still live
  // Remove newest target
  // Blind write 2 documents
  // Add one of the blind write docs to oldest target (preserves it)
  // Remove some documents from middle target (bumps sequence number)
  // Add some documents from newest target to oldest target (preserves them)
  // Update a doc from middle target
  // Remove middle target
  // Do a blind write
  // GC up to but not including the removal of the middle target
  //
  // Expect:
  // All docs in oldest target are still around
  // One blind write is gone, the first one not added to oldest target
  // Documents removed from middle target are gone, except ones added to oldest target
  // Documents from newest target are gone, except

  id<FSTPersistence> persistence = [self newPersistence];
  User user("user");
  id<FSTMutationQueue> mutationQueue = [persistence mutationQueueForUser:user];
  id<FSTQueryCache> queryCache = [persistence queryCache];
  id<FSTRemoteDocumentCache> documentCache = [persistence remoteDocumentCache];

  NSMutableSet<FSTDocumentKey *> *expectedRetained = [NSMutableSet set];
  NSMutableSet<FSTDocumentKey *> *expectedRemoved = [NSMutableSet set];

  // Add oldest target and docs
  FSTQueryData *oldestTarget = persistence.run("Add oldest target and docs", [&]() -> FSTQueryData * {
    FSTQueryData *oldestTarget = [self nextTestQuery:persistence];
    DocumentKeySet oldestDocs{};

    for (int i = 0; i < 5; i++) {
      FSTDocument *doc = [self nextTestDocument];
      [expectedRetained addObject:doc.key];
      oldestDocs = oldestDocs.insert(doc.key);
      [documentCache addEntry:doc];
    }

    [queryCache addQueryData:oldestTarget];
    [queryCache addMatchingKeys:oldestDocs forTargetID:oldestTarget.targetID];
    return oldestTarget;
  });

  // Add middle target and docs. Some docs will be removed from this target later.
  DocumentKeySet middleDocsToRemove{};
  FSTDocumentKey *middleDocToUpdate = nil;
  FSTQueryData *middleTarget = persistence.run("Add middle target and docs", [&]() -> FSTQueryData * {
    FSTQueryData *middleTarget = [self nextTestQuery:persistence];
    [queryCache addQueryData:middleTarget];
    DocumentKeySet middleDocs{};
    // these docs will be removed from this target later
    for (int i = 0; i < 2; i++) {
      FSTDocument *doc = [self nextTestDocument];
      [expectedRemoved addObject:doc.key];
      middleDocs = middleDocs.insert(doc.key);
      [documentCache addEntry:doc];
      middleDocsToRemove = middleDocsToRemove.insert(doc.key);
    }
    // these docs stay in this target and only this target
    for (int i = 2; i < 4; i++) {
      FSTDocument *doc = [self nextTestDocument];
      [expectedRetained addObject:doc.key];
      middleDocs = middleDocs.insert(doc.key);
      [documentCache addEntry:doc];
    }
    // This doc stays in this target, but gets updated
    {
      FSTDocument *doc = [self nextTestDocument];
      [expectedRetained addObject:doc.key];
      middleDocs = middleDocs.insert(doc.key);
      [documentCache addEntry:doc];
      middleDocToUpdate = doc.key;
    }
    [queryCache addMatchingKeys:middleDocs forTargetID:middleTarget.targetID];
    return middleTarget;
  });

  // Add newest target and docs.
  DocumentKeySet newestDocsToAddToOldest{};
  FSTQueryData *newestTarget = persistence.run("Add newest target and docs", [&]() -> FSTQueryData * {
    FSTQueryData *newestTarget = [self nextTestQuery:persistence];
    [queryCache addQueryData:newestTarget];
    DocumentKeySet newestDocs{};
    for (int i = 0; i < 3; i++) {
      FSTDocument *doc = [self nextBigTestDocument];
      [expectedRemoved addObject:doc.key];
      newestDocs = newestDocs.insert(doc.key);
      [documentCache addEntry:doc];
    }
    // docs to add to the oldest target, will be retained
    for (int i = 3; i < 5; i++) {
      FSTDocument *doc = [self nextBigTestDocument];
      [expectedRetained addObject:doc.key];
      newestDocs = newestDocs.insert(doc.key);
      newestDocsToAddToOldest = newestDocsToAddToOldest.insert(doc.key);
      [documentCache addEntry:doc];
    }
    [queryCache addMatchingKeys:newestDocs forTargetID:newestTarget.targetID];
    return newestTarget;
  });

  // newestTarget removed here, this should bump sequence number? maybe?
  // we don't really need the sequence number for anything, we just don't include it
  // in live queries.
  // TODO(gsoltis): probably don't need this
  //[self nextSequenceNumber];

  DocumentKeySet docKeys{};
  FSTDocument *doc1 = [self nextTestDocument];
  FSTDocument *doc2 = [self nextTestDocument];
  // 2 doc writes, add one of them to the oldest target.
  persistence.run("2 doc writes", [&]() {
    // write two docs and have them ack'd by the server. can skip mutation queue
    // and set them in document cache. Add potentially orphaned first, also add one
    // doc to a target.
    [documentCache addEntry:doc1];
    docKeys = docKeys.insert(doc1.key);
    //DocumentKeySet firstKey = docKeys;

    [documentCache addEntry:doc2];
    docKeys = docKeys.insert(doc2.key);
  });
  XCTAssertEqual(docKeys.size(), 2);

  persistence.run("ack the doc writes", [&]() {
    for (const DocumentKey &key : docKeys) {
      [persistence.referenceDelegate removeMutationReference:key];
    }
  });

  persistence.run("update target", [&]() {

    //[queryCache addPotentiallyOrphanedDocuments:docKeys atSequenceNumber:[self nextSequenceNumber]];

    NSData *token = [@"hello" dataUsingEncoding:NSUTF8StringEncoding];
    FSTListenSequenceNumber sequenceNumber = persistence.referenceDelegate.sequenceNumber;
    oldestTarget = [oldestTarget queryDataByReplacingSnapshotVersion:oldestTarget.snapshotVersion
                                                         resumeToken:token
                                                      sequenceNumber:sequenceNumber];
    [queryCache updateQueryData:oldestTarget];

    DocumentKeySet firstKey{doc1.key};
    [queryCache addMatchingKeys:firstKey forTargetID:oldestTarget.targetID];
    // nothing is keeping doc2 around, it should be removed
    [expectedRemoved addObject:doc2.key];
    // doc1 should be retained by being added to oldestTarget.
    [expectedRetained addObject:doc1.key];
  });

  // Remove some documents from the middle target.
  persistence.run("Remove some documents from the middle target", [&]() {
    FSTListenSequenceNumber sequenceNumber = persistence.referenceDelegate.sequenceNumber;
    NSData *token = [@"token" dataUsingEncoding:NSUTF8StringEncoding];
    middleTarget = [middleTarget queryDataByReplacingSnapshotVersion:middleTarget.snapshotVersion
                                                         resumeToken:token
                                                      sequenceNumber:sequenceNumber];

    [queryCache updateQueryData:middleTarget];
    [queryCache removeMatchingKeys:middleDocsToRemove forTargetID:middleTarget.targetID];
  });

  // Add a couple docs from the newest target to the oldest (preserves them past the point where
  // newest was removed)
  FSTListenSequenceNumber upperBound = persistence.run("Add a couple docs from the newest target to the oldest", [&]() -> FSTListenSequenceNumber {
    NSData *token = [@"add documents" dataUsingEncoding:NSUTF8StringEncoding];
    FSTListenSequenceNumber sequenceNumber = persistence.referenceDelegate.sequenceNumber;
    oldestTarget = [oldestTarget queryDataByReplacingSnapshotVersion:oldestTarget.snapshotVersion
                                                         resumeToken:token
                                                      sequenceNumber:sequenceNumber];
    [queryCache updateQueryData:oldestTarget];
    [queryCache addMatchingKeys:newestDocsToAddToOldest forTargetID:oldestTarget.targetID];
    return sequenceNumber;
  });

  // the sequence number right before middleTarget is updated, then removed.
  //FSTListenSequenceNumber upperBound = [self nextSequenceNumber];

  // Update a doc in the middle target
  persistence.run("Update a doc in the middle target", [&]() {
    FSTTestSnapshotVersion version = 3;
    FSTDocument *doc = [FSTDocument documentWithData:_testValue
                                                 key:middleDocToUpdate
                                             version:testutil::Version(version)
                                   hasLocalMutations:NO];
    [documentCache addEntry:doc];
    NSData *token = [@"updated" dataUsingEncoding:NSUTF8StringEncoding];
    FSTListenSequenceNumber sequenceNumber = persistence.referenceDelegate.sequenceNumber;
    middleTarget = [middleTarget queryDataByReplacingSnapshotVersion:middleTarget.snapshotVersion
                                                         resumeToken:token
                                                      sequenceNumber:sequenceNumber];
    [queryCache updateQueryData:middleTarget];
  });

  // middleTarget removed here
  //[self nextSequenceNumber];

  // Write a doc and get an ack, not part of a target
  persistence.run("Write a doc and get an ack, not part of a target", [&]() {
    FSTDocument *doc = [self nextTestDocument];

    [documentCache addEntry:doc];
    DocumentKeySet docKey{doc.key};
    // This should be retained, it's too new to get removed.
    [expectedRetained addObject:doc.key];
    //[queryCache addPotentiallyOrphanedDocuments:docKey atSequenceNumber:sequenceNumber];
    [persistence.referenceDelegate removeMutationReference:doc.key];
  });

  long sizeBefore = [self compactedSize:persistence];

  // Finally, do the garbage collection, up to but not including the removal of middleTarget
  persistence.run(
      "do the garbage collection, up to but not including the removal of middleTarget", [&]() {
        NSMutableDictionary<NSNumber *, FSTQueryData *> *liveQueries =
            [NSMutableDictionary dictionary];
        liveQueries[@(oldestTarget.targetID)] = oldestTarget;
        FSTLRUGarbageCollector *gc = [self gcForPersistence:persistence];
        NSUInteger queriesRemoved =
            [gc removeQueriesUpThroughSequenceNumber:upperBound liveQueries:liveQueries];
        XCTAssertEqual(1, queriesRemoved, @"Expected to remove newest target");

        NSUInteger docsRemoved = [gc removeOrphanedDocuments:documentCache
                                       throughSequenceNumber:upperBound
                                               mutationQueue:mutationQueue];
        NSLog(@"Expected removed: %@", expectedRemoved);
        NSLog(@"Expected retained: %@", expectedRetained);
        XCTAssertEqual([expectedRemoved count], docsRemoved);

        for (FSTDocumentKey *key in expectedRemoved) {
          XCTAssertNil([documentCache entryForKey:key],
                       @"Did not expect to find %@ in document cache", key);
          XCTAssertFalse([queryCache containsKey:key], @"Did not expect to find %@ in queryCache",
                         key);
        }
        for (FSTDocumentKey *key in expectedRetained) {
          XCTAssertNotNil([documentCache entryForKey:key], @"Expected to find %@ in document cache",
                          key);
        }
      });

  long sizeAfter = [self compactedSize:persistence];
  // Actual size difference will vary by persistence layer. In addtion,
  // we need to compact the leveldb persistence to get a read on size at this small of
  // an amount of data.
  XCTAssertLessThan(sizeAfter, sizeBefore);
  [persistence shutdown];
}
/*
- (void)testShouldGC {
  if ([self isTestBaseClass]) return;

  id<FSTPersistence> persistence = [self newPersistence];
  id<FSTQueryCache> queryCache = [persistence queryCache];
  id<FSTRemoteDocumentCache> docCache = [persistence remoteDocumentCache];
  User user("user");
  id<FSTMutationQueue> mutationQueue = [persistence mutationQueueForUser:user];
  FSTLRUThreshold thresholds{.min_ms_since_start = 1000,
                             .max_bytes_stored = 128,
                             .min_ms_between_attempts = 500,
                             .percentile_to_gc = 10};
  FSTLRUGarbageCollector *gc =
      [[FSTLRUGarbageCollector alloc] initWithQueryCache:queryCache thresholds:thresholds now:0];
  // It's too soon, we should not GC yet.
  XCTAssertFalse([gc shouldGCAt:0 currentSize:1024]);

  XCTAssertTrue([gc shouldGCAt:1001 currentSize:1024]);

  persistence.run("gc 1", [&]() {
    [gc collectGarbageWithLiveQueries:@{} documentCache:docCache mutationQueue:mutationQueue];
  });
  NSDate *now = [NSDate date];
  long nowMs = (long)([now timeIntervalSince1970] * 1000);
  // Too recent compared to the GC we just did.
  XCTAssertFalse([gc shouldGCAt:nowMs currentSize:1024]);

  // Enough time has passed
  XCTAssertTrue([gc shouldGCAt:nowMs + 501 currentSize:1024]);

  // Enough time has passed, but the stored byte size is too small
  XCTAssertFalse([gc shouldGCAt:nowMs + 501 currentSize:10]);
}
*/
@end

NS_ASSUME_NONNULL_END