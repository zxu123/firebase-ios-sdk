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

#import "one.h"

/**
 * The log levels used by FIRConfiguration.
 */
typedef NS_ENUM(NSInteger, FIRLogLevel) {
  /** Error */
  kFIRLogLevelError __deprecated = 0,
  /** Warning */
  kFIRLogLevelWarning __deprecated,
  /** Info */
  kFIRLogLevelInfo __deprecated,
  /** Debug */
  kFIRLogLevelDebug __deprecated,
  /** Assert */
  kFIRLogLevelAssert __deprecated,
  /** Max */
  kFIRLogLevelMax __deprecated = kFIRLogLevelAssert
} DEPRECATED_MSG_ATTRIBUTE(
    "Use -FIRDebugEnabled and -FIRDebugDisabled or setLoggerLevel. See FIRApp.h for more details.");

NS_ASSUME_NONNULL_BEGIN

/**
 * This interface provides global level properties that the developer can tweak, and the singleton
 * of the Firebase Analytics configuration class.
 */
@interface FIRConfiguration : NSObject

#if defined(__IPHONE_10_0) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_10_0
/** Returns the shared configuration object. */
@property(class, nonatomic, readonly) FIRConfiguration *sharedInstance;
#else
/** Returns the shared configuration object. */
+ (FIRConfiguration *)sharedInstance;
#endif  // defined(__IPHONE_10_0) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_10_0

/** The configuration class for Firebase Analytics. */
@property(nonatomic, readwrite) FIRAnalyticsConfiguration *analyticsConfiguration;

@end

NS_ASSUME_NONNULL_END
