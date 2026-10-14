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

#ifndef KEEPASSXC_AUTOFILLPLATFORM_H
#define KEEPASSXC_AUTOFILLPLATFORM_H

#include "AutofillCredentials.h"

#include <QString>

#include <functional>

/**
 * The AppKit, Security and AuthenticationServices calls the AutoFill service needs.
 */
namespace AutofillPlatform
{
    // Socket in the App Group container, the one place the sandboxed extension can reach
    QString socketPath();
    // Whether the running binary's signature grants the App Group that holds the socket
    bool hasAppGroupEntitlement();

    // Whether the process on the other end of a connected Unix socket is our signed AutoFill extension
    bool isTrustedPeer(int socketDescriptor, QString* reason = nullptr);

    // Whether the macOS version has the identity store; passkeys and one-time codes need newer versions still
    bool isIdentityStoreAvailable();
    bool supportsPasskeys();
    bool supportsOneTimeCodes();

    // Whether KeePassXC is turned on in System Settings, reported on the main thread
    void queryProviderEnabled(QObject* context, std::function<void(bool enabled)> callback);

    // Replace everything KeePassXC has in the system's credential identity store
    void replaceIdentities(const QList<AutofillIdentity>& identities);
    void removeAllIdentities();

    // System Settings pane where the user turns AutoFill providers on
    void openProviderSettings();
} // namespace AutofillPlatform

#endif // KEEPASSXC_AUTOFILLPLATFORM_H
