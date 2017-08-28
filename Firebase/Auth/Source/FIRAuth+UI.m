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

#import "FIRAuth+UI.h"

#import "FIRAuth_Internal.h"
#import "FIRAuthErrorUtils.h"

@import SafariServices;

NS_ASSUME_NONNULL_BEGIN

@implementation FIRAuth (UI)

- (BOOL)verifyAppWithURL:(NSURL *)URL
              UIDelegate:(nullable id<FIRAuthUIDelegate>)UIDelegate
                   error:(NSError **_Nullable)error {
  // If a UIDelegate is not provided.
  if (!UIDelegate) {
    UIViewController *topController =
        [UIApplication sharedApplication].keyWindow.rootViewController;
      [self presentWebContextWithController:topController URL:URL];
      return YES;
  }
  // If an invalid UIDelegae is provided.
  if (![self isValidUIDelegate:UIDelegate]) {
    *error = [FIRAuthErrorUtils invalidUIDelegateErrorWithMessage:nil];
    return NO;
  }

  // If a valid UIDelegate is provided.
  [self presentWebContextWithController:UIDelegate URL:URL];
  return YES;
}

/** @fn presentWebContextWithController:URL:
    @brief Presents a SFSafariViewController or WKWebView to display the contents of the URL
        provided.
    @param controller The controller used to present the SFSafariViewController or WKWebView.
    @param URL The URL to display in the SFSafariViewController or WKWebView.
 */
- (void)presentWebContextWithController:(id)controller URL:(NSURL *)URL {
#if HAS_SAFARI_VIEW_CONTROLLER
  SFSafariViewController *reCAPTCHAViewController =
      [[SFSafariViewController alloc] initWithURL:URL];
  [controller presentViewController:reCAPTCHAViewController animated:YES completion:nil];
  return;
#endif
}

/** @fn isValidUIDelegate
    @brief Determines whether the UI delegate is valid or not.
    @param UIDelegate The UI delegate to validate.
    @return Whether the UIDelegate is valid or not.
 */
- (BOOL)isValidUIDelegate:(id <FIRAuthUIDelegate>)UIDelegate {
  return ([UIDelegate isKindOfClass:[UIViewController class]] ||
      ([UIDelegate respondsToSelector:@selector(presentViewController:animated:completion:)] &&
      [UIDelegate respondsToSelector:@selector(dismissViewControllerAnimated:completion:)]));
}

@end

NS_ASSUME_NONNULL_END
