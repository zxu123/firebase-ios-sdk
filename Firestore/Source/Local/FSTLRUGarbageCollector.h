#import <Foundation/Foundation.h>

#import "Firestore/Source/Core/FSTTypes.h"
#import "Firestore/Source/Local/FSTQueryData.h"
#import "Firestore/Source/Local/FSTRemoteDocumentCache.h"

@protocol FSTPersistence;
@protocol FSTQueryCache;

extern const FSTListenSequenceNumber kFSTListenSequenceNumberInvalid;

struct FSTLRUThreshold {
  long min_ms_since_start;
  long max_bytes_stored;
  long min_ms_between_attempts;
  NSUInteger percentile_to_gc;
  static const FSTLRUThreshold &Defaults() {
    static const FSTLRUThreshold defaults = ([]() {
      FSTLRUThreshold thresholds;
      // 5 minutes
      thresholds.min_ms_since_start = 1000 * 60 * 5;
      // 10mb
      thresholds.max_bytes_stored = 1024 * 1024 * 10;
      // 1 minute
      thresholds.min_ms_between_attempts = 1000 * 60;
      thresholds.percentile_to_gc = 10;
      return thresholds;
    })();
    return defaults;
  }
};

@protocol FSTLRUDelegate

- (void)enumerateTargetsUsingBlock:(void (^)(FSTQueryData *queryData, BOOL *stop))block;

- (void)enumerateMutationsUsingBlock:(void (^)(FSTDocumentKey *key, FSTListenSequenceNumber sequenceNumber, BOOL *stop))block;

- (void)removeOrphanedDocumentsThroughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber;

- (NSUInteger)removeQueriesThroughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber liveQueries:(NSDictionary<NSNumber *, FSTQueryData *> *)liveQueries;

@end

@interface FSTLRUGarbageCollector : NSObject

- (instancetype)initWithQueryCache:(id<FSTQueryCache>)queryCache
                          delegate:(id<FSTLRUDelegate>)delegate
                        thresholds:(FSTLRUThreshold)thresholds
                               now:(long)now;

- (NSUInteger)queryCountForPercentile:(NSUInteger)percentile;

- (FSTListenSequenceNumber)sequenceNumberForQueryCount:(NSUInteger)queryCount;

- (NSUInteger)removeQueriesUpThroughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber
                                       liveQueries:
                                           (NSDictionary<NSNumber *, FSTQueryData *> *)liveQueries;

- (NSUInteger)removeOrphanedDocuments:(id<FSTRemoteDocumentCache>)remoteDocumentCache
                throughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber
                        mutationQueue:(id<FSTMutationQueue>)mutationQueue;

- (BOOL)shouldGCAt:(long)now currentSize:(long)currentSize;

- (void)collectGarbageWithLiveQueries:(NSDictionary<NSNumber *, FSTQueryData *> *)liveQueries
                        documentCache:(id<FSTRemoteDocumentCache>)docCache
                        mutationQueue:(id<FSTMutationQueue>)mutationQueue;

@end