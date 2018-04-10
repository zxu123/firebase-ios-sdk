#import <Firestore/Source/Local/FSTLevelDB.h>
#import "Firestore/Example/Tests/Local/FSTLRUGarbageCollectorTests.h"
#import "Firestore/Example/Tests/Local/FSTPersistenceTestHelpers.h"
#import "Firestore/Source/Local/FSTLRUGarbageCollector.h"

NS_ASSUME_NONNULL_BEGIN

@interface FSTLevelDBLRUGarbageCollectorTests : FSTLRUGarbageCollectorTests
@end

@implementation FSTLevelDBLRUGarbageCollectorTests

- (id<FSTPersistence>)newPersistence {
  return [FSTPersistenceTestHelpers levelDBPersistence];
}

- (long)compactedSize:(id<FSTPersistence>)persistence {
  // We do this in tests because otherwise at small data sizes there's too much variance
  // to assess GC effectiveness.
  ((FSTLevelDB *)persistence).ptr->CompactRange(NULL, NULL);
  return [persistence byteSize];
}

@end

NS_ASSUME_NONNULL_END