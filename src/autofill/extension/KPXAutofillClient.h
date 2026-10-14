/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

extern NSErrorDomain const KPXAutofillErrorDomain;

typedef NS_ERROR_ENUM(KPXAutofillErrorDomain, KPXAutofillError){
    // KeePassXC isn't running, or has AutoFill turned off so it doesn't listen
    KPXAutofillErrorNotRunning = 1,
    KPXAutofillErrorTimedOut,
    KPXAutofillErrorProtocol,
};

typedef void (^KPXAutofillCompletion)(NSDictionary* _Nullable response, NSError* _Nullable error);

/**
 * Talks to KeePassXC over the socket in the App Group container, in the frame
 * format of src/autofill/AutofillProtocol.h. Each request uses its own
 * connection; KeePassXC checks the code signature of every connection.
 */
@interface KPXAutofillClient : NSObject

// Sends a request off the main thread and calls back on the main thread
+ (void)send:(NSDictionary*)request timeout:(NSTimeInterval)timeout completion:(KPXAutofillCompletion)completion;

// Error string of a response whose "ok" is false, or nil for a successful response
+ (nullable NSString*)errorOfResponse:(NSDictionary*)response;

// Opens the KeePassXC app that contains this extension
+ (void)launchKeePassXC:(void (^)(BOOL launched))completion;

@end

NS_ASSUME_NONNULL_END
