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

#include "AutofillPlatform.h"
#include "autofill-config.h"

#include <QDebug>
#include <QDir>
#include <QPointer>

#import <AppKit/AppKit.h>
#import <AuthenticationServices/AuthenticationServices.h>
#import <Security/Security.h>

#include <sys/socket.h>
#include <sys/un.h>

// Compiled with ARC, see src/autofill/CMakeLists.txt

namespace
{
    NSString* toNSString(const QString& string)
    {
        return string.toNSString();
    }

    NSData* toNSData(const QByteArray& bytes)
    {
        return [NSData dataWithBytes:bytes.constData() length:static_cast<NSUInteger>(bytes.size())];
    }

    ASCredentialServiceIdentifier* domainIdentifier(const QString& host) API_AVAILABLE(macos(11.0))
    {
        return [[ASCredentialServiceIdentifier alloc] initWithIdentifier:toNSString(host)
                                                                    type:ASCredentialServiceIdentifierTypeDomain];
    }

    // Store calls fail while the user has not turned KeePassXC on, so check first
    void whenStoreEnabled(void (^block)(ASCredentialIdentityStore* store)) API_AVAILABLE(macos(11.0))
    {
        auto store = [ASCredentialIdentityStore sharedStore];
        [store getCredentialIdentityStoreStateWithCompletion:^(ASCredentialIdentityStoreState* state) {
            if (state.enabled) {
                block(store);
            }
        }];
    }

    void logStoreResult(BOOL success, NSError* error)
    {
        if (!success) {
            qWarning() << "AutoFill: updating the credential identity store failed:"
                       << QString::fromNSString(error.localizedDescription);
        }
    }
} // namespace

namespace AutofillPlatform
{
    QString socketPath()
    {
        NSString* group = @KPXC_AUTOFILL_APP_GROUP;
        NSURL* container = [[NSFileManager defaultManager] containerURLForSecurityApplicationGroupIdentifier:group];
        QString directory = container ? QString::fromNSString(container.path)
                                      : QDir::homePath() + "/Library/Group Containers/" + QString::fromNSString(group);
        QDir().mkpath(directory);

        const auto path = directory + "/autofill.sock";
        if (path.toUtf8().size() >= static_cast<int>(sizeof(sockaddr_un::sun_path))) {
            qWarning() << "AutoFill: the socket path is too long for a Unix socket:" << path;
        }
        return path;
    }

    bool hasAppGroupEntitlement()
    {
        SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
        if (!task) {
            return false;
        }
        CFTypeRef groups = SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.security.application-groups"), nullptr);
        CFRelease(task);
        if (!groups) {
            return false;
        }

        bool entitled = false;
        if (CFGetTypeID(groups) == CFArrayGetTypeID()) {
            auto array = static_cast<CFArrayRef>(groups);
            entitled = CFArrayContainsValue(array, CFRangeMake(0, CFArrayGetCount(array)), CFSTR(KPXC_AUTOFILL_APP_GROUP));
        }
        CFRelease(groups);
        return entitled;
    }

    bool isTrustedPeer(int socketDescriptor, QString* reason)
    {
        auto fail = [&](const QString& why) {
            if (reason) {
                *reason = why;
            }
            return false;
        };

        audit_token_t token;
        socklen_t length = sizeof(token);
        if (getsockopt(socketDescriptor, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &length) != 0) {
            return fail(QStringLiteral("could not read the peer's audit token"));
        }

        // Look the peer up by audit token rather than pid, which a process could recycle
        NSDictionary* attributes = @{(__bridge NSString*)kSecGuestAttributeAudit : [NSData dataWithBytes:&token
                                                                                                   length:length]};
        SecCodeRef code = nullptr;
        OSStatus status =
            SecCodeCopyGuestWithAttributes(nullptr, (__bridge CFDictionaryRef)attributes, kSecCSDefaultFlags, &code);
        if (status != errSecSuccess || !code) {
            return fail(QStringLiteral("could not find the peer's code signature (%1)").arg(status));
        }

        SecRequirementRef requirement = nullptr;
        status = SecRequirementCreateWithString(CFSTR(KPXC_AUTOFILL_PEER_REQUIREMENT), kSecCSDefaultFlags, &requirement);
        if (status != errSecSuccess || !requirement) {
            CFRelease(code);
            return fail(QStringLiteral("invalid code requirement (%1)").arg(status));
        }

        status = SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement);
        CFRelease(requirement);
        CFRelease(code);
        if (status != errSecSuccess) {
            return fail(QStringLiteral("the peer does not satisfy the code requirement (%1)").arg(status));
        }
        return true;
    }

    bool isIdentityStoreAvailable()
    {
        if (@available(macOS 11.0, *)) {
            return true;
        }
        return false;
    }

    bool supportsPasskeys()
    {
        if (@available(macOS 14.0, *)) {
            return true;
        }
        return false;
    }

    bool supportsOneTimeCodes()
    {
        if (@available(macOS 15.0, *)) {
            return true;
        }
        return false;
    }

    void queryProviderEnabled(QObject* context, std::function<void(bool enabled)> callback)
    {
        QPointer<QObject> guard(context);
        if (@available(macOS 11.0, *)) {
            [[ASCredentialIdentityStore sharedStore]
                getCredentialIdentityStoreStateWithCompletion:^(ASCredentialIdentityStoreState* state) {
                    const bool enabled = state.enabled;
                    // Qt's GUI thread is the main thread on macOS
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (guard) {
                            callback(enabled);
                        }
                    });
                }];
        } else {
            callback(false);
        }
    }

    void replaceIdentities(const QList<AutofillIdentity>& identities)
    {
        if (@available(macOS 14.0, *)) {
            NSMutableArray<id<ASCredentialIdentity>>* entries = [NSMutableArray array];
            for (const auto& identity : identities) {
                switch (identity.type) {
                case AutofillIdentity::Type::Password:
                    [entries addObject:[[ASPasswordCredentialIdentity alloc]
                                           initWithServiceIdentifier:domainIdentifier(identity.service)
                                                                user:toNSString(identity.user)
                                                    recordIdentifier:toNSString(identity.recordId)]];
                    break;
                case AutofillIdentity::Type::Passkey:
                    [entries addObject:[[ASPasskeyCredentialIdentity alloc]
                                           initWithRelyingPartyIdentifier:toNSString(identity.service)
                                                                 userName:toNSString(identity.user)
                                                             credentialID:toNSData(identity.credentialId)
                                                               userHandle:toNSData(identity.userHandle)
                                                         recordIdentifier:toNSString(identity.recordId)]];
                    break;
                case AutofillIdentity::Type::OneTimeCode:
                    if (@available(macOS 15.0, *)) {
                        [entries addObject:[[ASOneTimeCodeCredentialIdentity alloc]
                                               initWithServiceIdentifier:domainIdentifier(identity.service)
                                                                   label:toNSString(identity.user)
                                                        recordIdentifier:toNSString(identity.recordId)]];
                    }
                    break;
                }
            }
            whenStoreEnabled(^(ASCredentialIdentityStore* store) {
                [store replaceCredentialIdentityEntries:entries
                                             completion:^(BOOL success, NSError* error) {
                                                 logStoreResult(success, error);
                                             }];
            });
        } else if (@available(macOS 11.0, *)) {
            // Before macOS 14 the store holds passwords only
            NSMutableArray<ASPasswordCredentialIdentity*>* entries = [NSMutableArray array];
            for (const auto& identity : identities) {
                if (identity.type == AutofillIdentity::Type::Password) {
                    [entries addObject:[[ASPasswordCredentialIdentity alloc]
                                           initWithServiceIdentifier:domainIdentifier(identity.service)
                                                                user:toNSString(identity.user)
                                                    recordIdentifier:toNSString(identity.recordId)]];
                }
            }
            whenStoreEnabled(^(ASCredentialIdentityStore* store) {
                [store replaceCredentialIdentitiesWithIdentities:entries
                                                      completion:^(BOOL success, NSError* error) {
                                                          logStoreResult(success, error);
                                                      }];
            });
        }
    }

    void removeAllIdentities()
    {
        if (@available(macOS 11.0, *)) {
            whenStoreEnabled(^(ASCredentialIdentityStore* store) {
                [store removeAllCredentialIdentitiesWithCompletion:^(BOOL success, NSError* error) {
                    logStoreResult(success, error);
                }];
            });
        }
    }

    void openProviderSettings()
    {
        if (@available(macOS 15.0, *)) {
            [ASSettingsHelper openCredentialProviderAppSettingsWithCompletionHandler:^(NSError* error) {
                if (error) {
                    qWarning() << "AutoFill: could not open the AutoFill settings:"
                               << QString::fromNSString(error.localizedDescription);
                }
            }];
            return;
        }
        // Before macOS 15 the providers are listed among the extensions
        [[NSWorkspace sharedWorkspace]
            openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preferences.extensions"]];
    }
} // namespace AutofillPlatform
