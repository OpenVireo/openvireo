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

#ifndef KEEPASSXC_AUTHCLIENT_H
#define KEEPASSXC_AUTHCLIENT_H

#include "config-keepassx.h"

#include "gui/remote/RemoteSettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Asynchronous JSON client for the KeePassXC auth server (docs/plans/accounts/openapi.yaml), in the
 * style of SyncServerClient: every request ends by calling its callback on the client's thread,
 * unless abort() runs first, and tests override send() to answer without a network.
 *
 * Routes that need a signed-in caller get their bearer token from the token provider; the others
 * (registration, login, refresh, service account exchange, discovery) never send one.
 */
class AuthClient : public QObject
{
    Q_OBJECT

public:
    struct Response
    {
        // HTTP status, 0 when the request never got a response
        int status = 0;
        QByteArray body;
        QString networkError;

        bool isSuccess() const
        {
            return status >= 200 && status < 300;
        }
        QJsonObject json() const;
        // The machine readable error code, e.g. token_reused
        QString errorCode() const;
    };
    using Callback = std::function<void(const Response&)>;
    using Headers = QList<QPair<QByteArray, QByteArray>>;

    struct DeviceInfo
    {
        // Empty on this device's first sign-in; the server assigns one
        QString id;
        QString name;
        // qt-linux, qt-macos or qt-windows
        QString platform;

        QJsonObject toJson() const;
        static DeviceInfo current(const QString& id = {});
    };

    static constexpr int TimeoutMsec = 30000;

    // `apiUrl` is the auth server's API, e.g. https://accounts.keepassxc.org/api/v1
    explicit AuthClient(const QString& apiUrl = {}, QObject* parent = nullptr);
    ~AuthClient() override;

    void setApiUrl(const QString& apiUrl);
    QString apiUrl() const;
    void setTokenProvider(AccessTokenProvider provider);

    // GET <issuer>/.well-known/kpx-configuration: {issuer, api, sync, jwks_uri, opaque, kdf_min}
    void discover(const QString& issuerUrl, Callback callback);

    void registerStart(const QString& email, const QByteArray& request, Callback callback);
    void registerFinish(const QString& sessionId,
                        const QByteArray& record,
                        const QJsonObject& kdf,
                        const QJsonObject& keyset,
                        const DeviceInfo& device,
                        Callback callback);
    void loginStart(const QString& email, const QByteArray& ke1, Callback callback);
    // A Session, or {mfa_required: true, mfa_token, methods}
    void loginFinish(const QString& sessionId, const QByteArray& authU, const DeviceInfo& device, Callback callback);
    void loginMfa(const QString& mfaToken, const QString& totp, Callback callback);
    void refresh(const QString& refreshToken, Callback callback);
    void logout(Callback callback);
    void exchangeServiceAccount(const QString& saId, const QString& secret, Callback callback);

    void me(Callback callback);
    void lookupUser(const QString& email, Callback callback);
    // `principal` is user:<id> or sa:<id>
    void publicKey(const QString& principal, Callback callback);
    void listDevices(Callback callback);
    void revokeDevice(const QString& deviceId, Callback callback);

    void listVaults(Callback callback);
    void getVault(const QString& vaultId, Callback callback);
    void createVault(const QString& vaultId,
                     const QString& orgId,
                     const QByteArray& encryptedName,
                     const QJsonArray& grants,
                     Callback callback);
    void renameVault(const QString& vaultId, const QByteArray& encryptedName, Callback callback);
    void deleteVault(const QString& vaultId, Callback callback);
    void vaultAccess(const QString& vaultId, Callback callback);
    void grantAccess(const QString& vaultId, const QJsonArray& grants, Callback callback);
    // Start a key rotation (SPEC §7): `grants` for every remaining member, `remove` the principals to drop
    void startRotation(const QString& vaultId,
                       int keyVersion,
                       const QJsonArray& grants,
                       const QStringList& remove,
                       Callback callback);
    void activateKey(const QString& vaultId, int keyVersion, Callback callback);

    // Orgs (the caller's orgs and roles come with me())
    // A new org; the caller becomes its owner
    void createOrg(const QString& name, Callback callback);
    void orgMembers(const QString& orgId, Callback callback);
    // A new role for a member. `saMasterKeys` (SaMasterGrant) go along when promoting to owner or admin.
    void setMemberRole(const QString& orgId,
                       const QString& userId,
                       const QString& role,
                       const QJsonArray& saMasterKeys,
                       Callback callback);
    // The server deletes their vault access and flags the org vaults they could open (rotation_required)
    void removeMember(const QString& orgId, const QString& userId, Callback callback);
    // Confirm an accepted member with their vault grants (VaultGrant) and, for admins, the SA master keys
    void confirmMember(const QString& orgId,
                       const QString& userId,
                       const QJsonArray& grants,
                       const QJsonArray& saMasterKeys,
                       Callback callback);
    // Invitations (owner/admin); the invitee gets the token by email only (SPEC §12)
    void listInvitations(const QString& orgId, Callback callback);
    void invite(const QString& orgId, const QString& email, const QString& role, Callback callback);
    void cancelInvitation(const QString& orgId, const QString& invitationId, Callback callback);
    // Accept as the signed-in user; an owner or admin confirms the member afterwards
    void acceptInvitation(const QString& token, Callback callback);

    // Service accounts of an org (owner/admin)
    void listServiceAccounts(const QString& orgId, Callback callback);
    // `body` is {id, name, expires_at, keyset, master_keys, secret}, see openapi.yaml
    void createServiceAccount(const QString& orgId, const QJsonObject& body, Callback callback);
    // Owners and admins also get their sealed copy of the master key (`master_key`)
    void getServiceAccount(const QString& saId, Callback callback);
    // Any of {name, expires_at, disabled}
    void updateServiceAccount(const QString& saId, const QJsonObject& changes, Callback callback);
    void deleteServiceAccount(const QString& saId, Callback callback);
    void listServiceAccountSecrets(const QString& saId, Callback callback);
    // `secret` is a NewSaSecret: {id, name, secret_hash, prefix, wrapped_master, expires_at}
    void issueServiceAccountSecret(const QString& saId, const QJsonObject& secret, Callback callback);
    // {expires_at}: an ISO time, or null for never; the secret's token stays the same
    void updateServiceAccountSecret(const QString& saId,
                                    const QString& secretId,
                                    const QJsonObject& changes,
                                    Callback callback);
    void revokeServiceAccountSecret(const QString& saId, const QString& secretId, Callback callback);

    // The org's audit log, newest first; `before` is the `next` cursor of the previous page
    void auditLog(const QString& orgId, const QString& before, int limit, Callback callback);

    // A Grant for POST /vaults, /vaults/{id}/access and /vaults/{id}/keys
    static QJsonObject
    grant(const QString& principal, const QString& role, int keyVersion, const QByteArray& sealedKey);

    // Drop all requests in flight; their callbacks are never called
    void abort();

    QUrl endpoint(const QString& path) const;
    static QString errorMessage(const Response& response);

protected:
    // Overridden by tests to answer without a network. Authenticated requests carry the Authorization header.
    virtual void send(const QByteArray& verb,
                      const QUrl& url,
                      const Headers& headers,
                      const QByteArray& body,
                      int timeoutMsec,
                      Callback callback);

    void deliverLater(const Response& response, Callback callback);

private:
    void post(const QString& path, const QJsonObject& body, Callback callback, bool authenticated = false);
    void get(const QString& path, Callback callback);
    void sendJson(const QByteArray& verb, const QString& path, const QJsonObject& body, Callback callback);
    void
    request(const QByteArray& verb, const QUrl& url, const QByteArray& body, bool authenticated, Callback callback);

    QString m_apiUrl;
    AccessTokenProvider m_tokenProvider;
    quint64 m_generation = 0;
#ifdef KPXC_FEATURE_NETWORK
    QNetworkAccessManager* m_nam = nullptr;
    QSet<QNetworkReply*> m_replies;
#endif

    Q_DISABLE_COPY(AuthClient)
};

#endif // KEEPASSXC_AUTHCLIENT_H
