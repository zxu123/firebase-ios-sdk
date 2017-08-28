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

#import <FirebaseCommunity/FirebaseCommunity.h>

#import "FIRAuthUIDelegate.h"

// SFSafariViewController only exists in iOS 9+ SDKs.
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 90000
#define HAS_SAFARI_VIEW_CONTROLLER 1
#endif

NS_ASSUME_NONNULL_BEGIN

@interface FIRAuth (UI)

/** @fn verifyAppWithURL:UIDelegate:error:
    @brief Attempts to verify the app via a mobile web context.
    @param URL The URL hosting the web page to verify the app.
    @param UIDelegate A view controller object used to present the SFSafariViewController or
        WKWebview.
    @param error The error that occurred, if any.
    @return Whether the web context was launched successfully or not.
 */
- (BOOL)verifyAppWithURL:(NSURL *)URL
              UIDelegate:(nullable id<FIRAuthUIDelegate>)UIDelegate
                   error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
