#import "Firestore/Source/Local/FSTLRUGarbageCollector.h"

#import <queue>

#import "Firestore/Source/Local/FSTMutationQueue.h"
#import "Firestore/Source/Local/FSTPersistence.h"
#import "Firestore/Source/Local/FSTQueryCache.h"
#import "Firestore/Source/Util/FSTLogger.h"

const FSTListenSequenceNumber kFSTListenSequenceNumberInvalid = -1;

class RollingSequenceNumberBuffer {
 public:
  RollingSequenceNumberBuffer(NSUInteger max_elements)
      : max_elements_(max_elements), queue_(std::priority_queue<FSTListenSequenceNumber>()) {
  }

  RollingSequenceNumberBuffer(const RollingSequenceNumberBuffer &other) = delete;
  RollingSequenceNumberBuffer(RollingSequenceNumberBuffer &other) = delete;

  RollingSequenceNumberBuffer &operator=(const RollingSequenceNumberBuffer &other) = delete;
  RollingSequenceNumberBuffer &operator=(RollingSequenceNumberBuffer &other) = delete;

  void AddElement(FSTListenSequenceNumber sequence_number) {
    if (queue_.size() < max_elements_) {
      queue_.push(sequence_number);
    } else {
      FSTListenSequenceNumber highestValue = queue_.top();
      if (sequence_number < highestValue) {
        queue_.pop();
        queue_.push(sequence_number);
      }
    }
  }

  FSTListenSequenceNumber max_value() const {
    return queue_.top();
  }

  std::size_t size() const {
    return queue_.size();
  }

 private:
  std::priority_queue<FSTListenSequenceNumber> queue_;
  const NSUInteger max_elements_;
};

@interface FSTLRUGarbageCollector ()

@property(nonatomic, strong, readonly) id<FSTQueryCache> queryCache;

@end

@implementation FSTLRUGarbageCollector {
  FSTLRUThreshold _threshold;
  long _lastGcTime;
  long _startTimeEpoch;
}

- (instancetype)initWithQueryCache:(id<FSTQueryCache>)queryCache thresholds:(FSTLRUThreshold)thresholds now:(long)now {
  self = [super init];
  if (self) {
    _queryCache = queryCache;
    _threshold = std::move(thresholds);
    _startTimeEpoch = now;
  }
  return self;
}

- (BOOL)shouldGCAt:(long)now persistence:(id<FSTPersistence>)persistence {
  long msSinceStart = now - _startTimeEpoch;
  if (msSinceStart < _threshold.min_ms_since_start) {
    return NO;
  }

  long elapsed = now - _lastGcTime;
  if (elapsed < _threshold.min_ms_between_attempts) {
    return NO;
  }

  long total_storage_size = [persistence byteSize];
  if (total_storage_size < _threshold.max_bytes_stored) {
    return NO;
  }

  return YES;
}

- (NSUInteger)queryCountForPercentile:(NSUInteger)percentile {
  NSUInteger totalCount = (NSUInteger)[self.queryCache count];
  NSUInteger setSize = (NSUInteger)((percentile / 100.0f) * totalCount);
  return setSize;
}

- (FSTListenSequenceNumber)sequenceNumberForQueryCount:(NSUInteger)queryCount {
  if (queryCount == 0) {
    return kFSTListenSequenceNumberInvalid;
  }
  RollingSequenceNumberBuffer buffer(queryCount);
  RollingSequenceNumberBuffer *ptr_to_buffer = &buffer;
  [self.queryCache
      enumerateTargetsUsingBlock:^(FSTQueryData *queryData, BOOL *stop) {
        ptr_to_buffer->AddElement(queryData.sequenceNumber);
      }];
  [self.queryCache enumerateOrphanedDocumentsUsingBlock:^(FSTDocumentKey *docKey,
          FSTListenSequenceNumber sequenceNumber, BOOL *stop) {
    ptr_to_buffer->AddElement(sequenceNumber);
  }];
  return buffer.max_value();
}

- (NSUInteger)removeQueriesUpThroughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber
                                       liveQueries:
                                           (NSDictionary<NSNumber *, FSTQueryData *> *)liveQueries {
  return [self.queryCache removeQueriesThroughSequenceNumber:sequenceNumber
                                                 liveQueries:liveQueries];
}

- (NSUInteger)removeOrphanedDocuments:(id<FSTRemoteDocumentCache>)remoteDocumentCache
                throughSequenceNumber:(FSTListenSequenceNumber)sequenceNumber
                        mutationQueue:(id<FSTMutationQueue>)mutationQueue {
  return [remoteDocumentCache removeOrphanedDocuments:self.queryCache
                                throughSequenceNumber:sequenceNumber
                                        mutationQueue:mutationQueue];
}

- (void)collectGarbageWithLiveQueries:(NSDictionary<NSNumber *, FSTQueryData *> *)liveQueries
                        documentCache:(id<FSTRemoteDocumentCache>)docCache
                        mutationQueue:(id<FSTMutationQueue>)mutationQueue {
  NSDate *startTime = [NSDate date];
  NSUInteger queryCount = [self queryCountForPercentile:_threshold.percentile_to_gc];
  FSTListenSequenceNumber sequenceNumber = [self sequenceNumberForQueryCount:queryCount];
  NSDate *boundaryTime = [NSDate date];
  NSUInteger queriesRemoved = [self removeQueriesUpThroughSequenceNumber:sequenceNumber
                                                             liveQueries:liveQueries];
  NSDate *queryRemovalTime = [NSDate date];
  NSUInteger documentsRemoved = [self removeOrphanedDocuments:docCache
                                        throughSequenceNumber:sequenceNumber
                                                mutationQueue:mutationQueue];
  NSDate *endTime = [NSDate date];
  int totalMs = (int)([endTime timeIntervalSinceDate:startTime] * 1000);
  int boundaryMs = (int)([boundaryTime timeIntervalSinceDate:startTime] * 1000);
  int queriesRemovedMs = (int)([queryRemovalTime timeIntervalSinceDate:boundaryTime] * 1000);
  int documentsRemovedMs = (int)([endTime timeIntervalSinceDate:queryRemovalTime] * 1000);
  NSMutableString *report = [NSMutableString string];
  [report appendFormat:@"Garbage collection finished in %ims", totalMs];
  [report appendFormat:@"\n - Identified %i%% sequence number in %ims", _threshold.percentile_to_gc, boundaryMs];
  [report appendFormat:@"\n - %i targets removed in %ims", queriesRemoved, queriesRemovedMs];
  [report appendFormat:@"\n - %i documents removed in %ims", documentsRemoved, documentsRemovedMs];
  FSTLog(@"%@", report);
}

@end