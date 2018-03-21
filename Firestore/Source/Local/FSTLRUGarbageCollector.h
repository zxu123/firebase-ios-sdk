#import <Foundation/Foundation.h>

#import "Firestore/Source/Core/FSTTypes.h"
#import "Firestore/Source/Local/FSTQueryData.h"
#import "Firestore/Source/Local/FSTRemoteDocumentCache.h"
#import "Firestore/Source/Local/FSTWriteGroup.h"

@protocol FSTQueryCache;

extern const FSTListenSequenceNumber kFSTListenSequenceNumberInvalid;

struct FSTLRUThreshold {
    long min_ms_since_start;
    long max_bytes_stored;
    long min_ms_between_attempts;
    NSUInteger percentile_to_gc;
};

@interface FSTLRUGarbageCollector : NSObject

- (instancetype)initWithQueryCache:(id<FSTQueryCache>)queryCache;

- (NSUInteger)queryCountForPercentile:(NSUInteger)percentile;

- (FSTListenSequenceNumber)sequenceNumberForQueryCount:(NSUInteger)queryCount;

- (NSUInteger)removeQueriesUpThroughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber
                                       liveQueries:
                                           (NSDictionary<NSNumber *, FSTQueryData *> *)liveQueries
                                             group:(FSTWriteGroup *)group;

- (NSUInteger)removeOrphanedDocuments:(id<FSTRemoteDocumentCache>)remoteDocumentCache
                throughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber
                        mutationQueue:(id<FSTMutationQueue>)mutationQueue
                                group:(FSTWriteGroup *)group;

@end