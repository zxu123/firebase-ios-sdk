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

#import "Firestore/Source/Local/FSTMemoryRemoteDocumentCache.h"

#import "Firestore/Source/Model/FSTDocument.h"
#import "Firestore/Source/Model/FSTFieldValue.h"
#import "Firestore/Source/Local/FSTMemoryPersistence.h"

#import "Firestore/Example/Tests/Local/FSTPersistenceTestHelpers.h"
#import "Firestore/Example/Tests/Local/FSTRemoteDocumentCacheTests.h"
#import "Firestore/Example/Tests/Util/FSTHelpers.h"

@interface FSTMemoryRemoteDocumentCacheTests : FSTRemoteDocumentCacheTests
@end

/**
 * The tests for FSTMemoryRemoteDocumentCache are performed on the FSTRemoteDocumentCache
 * protocol in FSTRemoteDocumentCacheTests. This class is merely responsible for setting up and
 * tearing down the @a remoteDocumentCache.
 */
@implementation FSTMemoryRemoteDocumentCacheTests

- (void)setUp {
  [super setUp];

  self.persistence = [FSTPersistenceTestHelpers memoryPersistence];
  self.remoteDocumentCache = [self.persistence remoteDocumentCache];
}

- (void)tearDown {
  [self.remoteDocumentCache shutdown];
  self.persistence = nil;
  self.remoteDocumentCache = nil;

  [super tearDown];
}

- (void)testByteSize {
  self.persistence.run("testByteSize", [&]() {
    FSTDocument *doc = FSTTestDoc("a/b", 1, @{
            @"key1": @1,  // 4 bytes + 8 bytes
            @"key2": @{   // 4 bytes
                    @"isBool": @YES,  // 6 bytes + 1 byte
                    @"isString": @"also yes"  // 8 bytes + 8 bytes
            }
    }, NO);
    [self.remoteDocumentCache addEntry:doc];

    long bytes = [(FSTMemoryRemoteDocumentCache *)self.remoteDocumentCache byteSize];
    NSLog(@"bytes? %li", bytes);
  });
}

@end
