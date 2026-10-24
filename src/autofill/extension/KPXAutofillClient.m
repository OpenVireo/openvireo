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

#import "KPXAutofillClient.h"
#import "autofill-config.h"

#import <AppKit/AppKit.h>

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

NSErrorDomain const KPXAutofillErrorDomain = @"KPXAutofillErrorDomain";

// Same limit as AutofillProtocol::MaxFrameSize
static const uint32_t KPXMaxFrameSize = 1024 * 1024;

static NSError* KPXError(KPXAutofillError code, NSString* description)
{
    return [NSError errorWithDomain:KPXAutofillErrorDomain
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey : description}];
}

static NSString* KPXSocketPath(void)
{
    NSURL* container = [[NSFileManager defaultManager]
        containerURLForSecurityApplicationGroupIdentifier:@KPXC_AUTOFILL_APP_GROUP];
    return [container.path stringByAppendingPathComponent:@"autofill.sock"];
}

// Waits until the socket is ready or the deadline passes
static BOOL KPXWait(int fd, short events, NSDate* deadline)
{
    while (YES) {
        const NSTimeInterval remaining = [deadline timeIntervalSinceNow];
        if (remaining <= 0) {
            return NO;
        }
        struct pollfd pfd = {fd, events, 0};
        const int result = poll(&pfd, 1, (int)MIN(remaining * 1000, INT_MAX));
        if (result > 0) {
            return YES;
        }
        if (result < 0 && errno != EINTR) {
            return NO;
        }
    }
}

static BOOL KPXWriteAll(int fd, const uint8_t* bytes, size_t length, NSDate* deadline)
{
    size_t written = 0;
    while (written < length) {
        if (!KPXWait(fd, POLLOUT, deadline)) {
            return NO;
        }
        const ssize_t result = write(fd, bytes + written, length - written);
        if (result < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return NO;
        }
        written += (size_t)result;
    }
    return YES;
}

static BOOL KPXReadAll(int fd, uint8_t* bytes, size_t length, NSDate* deadline)
{
    size_t done = 0;
    while (done < length) {
        if (!KPXWait(fd, POLLIN, deadline)) {
            return NO;
        }
        const ssize_t result = read(fd, bytes + done, length - done);
        if (result == 0) {
            return NO;
        }
        if (result < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return NO;
        }
        done += (size_t)result;
    }
    return YES;
}

static NSDictionary* KPXSendSync(NSDictionary* request, NSTimeInterval timeout, NSError** error)
{
    NSString* path = KPXSocketPath();
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const char* cPath = path.fileSystemRepresentation;
    if (!path || strlen(cPath) >= sizeof(address.sun_path)) {
        *error = KPXError(KPXAutofillErrorNotRunning, @"The OpenVireo socket path is not usable.");
        return nil;
    }
    strlcpy(address.sun_path, cPath, sizeof(address.sun_path));

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        *error = KPXError(KPXAutofillErrorNotRunning, @"Could not create a socket.");
        return nil;
    }
    const int noSigPipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));

    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        close(fd);
        *error = KPXError(KPXAutofillErrorNotRunning, @"OpenVireo is not running.");
        return nil;
    }

    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    NSData* body = [NSJSONSerialization dataWithJSONObject:request options:0 error:nil];
    const uint32_t length = CFSwapInt32HostToBig((uint32_t)body.length);
    BOOL sent = KPXWriteAll(fd, (const uint8_t*)&length, sizeof(length), deadline)
                && KPXWriteAll(fd, body.bytes, body.length, deadline);
    if (!sent) {
        close(fd);
        *error = KPXError(KPXAutofillErrorNotRunning, @"OpenVireo closed the connection.");
        return nil;
    }

    uint32_t responseLength = 0;
    if (!KPXReadAll(fd, (uint8_t*)&responseLength, sizeof(responseLength), deadline)) {
        close(fd);
        *error = [deadline timeIntervalSinceNow] <= 0
                     ? KPXError(KPXAutofillErrorTimedOut, @"OpenVireo did not answer in time.")
                     : KPXError(KPXAutofillErrorNotRunning, @"OpenVireo closed the connection.");
        return nil;
    }
    responseLength = CFSwapInt32BigToHost(responseLength);
    if (responseLength == 0 || responseLength > KPXMaxFrameSize) {
        close(fd);
        *error = KPXError(KPXAutofillErrorProtocol, @"OpenVireo sent a malformed answer.");
        return nil;
    }

    NSMutableData* responseBody = [NSMutableData dataWithLength:responseLength];
    const BOOL received = KPXReadAll(fd, responseBody.mutableBytes, responseLength, deadline);
    close(fd);
    if (!received) {
        *error = KPXError(KPXAutofillErrorTimedOut, @"OpenVireo did not answer in time.");
        return nil;
    }

    id response = [NSJSONSerialization JSONObjectWithData:responseBody options:0 error:nil];
    // The answer may hold a password; wipe our copy of the raw bytes
    memset(responseBody.mutableBytes, 0, responseBody.length);
    if (![response isKindOfClass:[NSDictionary class]]) {
        *error = KPXError(KPXAutofillErrorProtocol, @"OpenVireo sent a malformed answer.");
        return nil;
    }
    return response;
}

@implementation KPXAutofillClient

+ (void)send:(NSDictionary*)request timeout:(NSTimeInterval)timeout completion:(KPXAutofillCompletion)completion
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError* error = nil;
        NSDictionary* response = KPXSendSync(request, timeout, &error);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(response, error);
        });
    });
}

+ (NSString*)errorOfResponse:(NSDictionary*)response
{
    if ([response[@"ok"] boolValue]) {
        return nil;
    }
    NSString* error = response[@"error"];
    return [error isKindOfClass:[NSString class]] ? error : @"failed";
}

+ (void)launchKeePassXC:(void (^)(BOOL launched))completion
{
    // The extension sits in KeePassXC.app/Contents/PlugIns
    NSURL* appURL = [[[[NSBundle mainBundle].bundleURL URLByDeletingLastPathComponent] URLByDeletingLastPathComponent]
        URLByDeletingLastPathComponent];
    NSWorkspaceOpenConfiguration* configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:appURL
                                          configuration:configuration
                                      completionHandler:^(NSRunningApplication* app, NSError* error) {
                                          dispatch_async(dispatch_get_main_queue(), ^{
                                              completion(app != nil && error == nil);
                                          });
                                      }];
}

@end
