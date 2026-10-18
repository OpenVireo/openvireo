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

#ifndef KEEPASSXC_FAKEACCOUNTSERVER_H
#define KEEPASSXC_FAKEACCOUNTSERVER_H

#include "account/AccountManager.h"
#include "account/AuthClient.h"
#include "account/OpaqueClient.h"
#include "gui/remote/SyncServerClient.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QSharedPointer>

/**
 * An in-memory KeePassXC account server and sync server for tests, following
 * docs/plans/accounts/openapi.yaml and sync-openapi.yaml closely enough for the clients not to notice.
 *
 * OPAQUE is replaced by comparing password hashes (FakeOpaqueClient), so these tests don't need
 * libopaque; TestAccountManager runs the real protocol. Access tokens are opaque strings, but like a JWT's
 * `vlt` claim each one remembers the vaults its principal could open when it was issued, and the sync
 * server only accepts it for those.
 */
namespace FakeAccount
{
    extern const QString Issuer;
    extern const QString SyncUrl;
    extern const QString SelfHostedUrl;
    extern const QString Totp;

    AccountManager::ServerConfig serverConfig();
    // The cheapest settings clients accept
    AccountCrypto::KdfParams testKdf();

    class FakeOpaqueClient : public OpaqueClient
    {
    public:
        bool createRegistrationRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* request) override;
        bool finalizeRegistration(const SecureBytes& sec,
                                  const QByteArray& response,
                                  const Ids& ids,
                                  QByteArray* record) override;
        bool createCredentialRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* ke1) override;
        bool recoverCredentials(const QByteArray& ke2,
                                const SecureBytes& sec,
                                const Ids& ids,
                                QByteArray* authU,
                                SecureBytes* sessionKey) override;
    };

    struct Server
    {
        struct User
        {
            QString id;
            QString email;
            QByteArray record;
            QJsonObject kdf;
            QJsonObject keyset;
            bool mfa = false;
        };
        struct Access
        {
            QString principal;
            QString role;
            int keyVersion = 0;
            QString sealedKey;
            QString grantedBy;
        };
        struct Vault
        {
            QString id;
            QString orgId;
            QString ownerId;
            QString encryptedName;
            int current = 1;
            int pending = -1;
            bool rotationRequired = false;
            QList<Access> access;
        };
        struct Blob
        {
            QByteArray data;
            QString etag;
            qint64 revision = 0;
        };
        struct OrgMember
        {
            QString userId;
            QString role;
            QString status = "confirmed";
        };
        struct Org
        {
            QString id;
            QString name;
            QList<OrgMember> members;
        };
        struct ServiceAccount
        {
            QJsonObject json;
            // principal -> sealed master key
            QHash<QString, QString> masterKeys;
        };
        struct SaSecret
        {
            QString id;
            QString saId;
            QString name;
            QString hash;
            QString prefix;
            QString wrappedMaster;
            QJsonValue expiresAt;
        };
        struct Token
        {
            QString principal;
            // The vaults the token covers, like a JWT's vlt claim: vault id -> role
            QHash<QString, QString> vaults;
        };

        QHash<QString, User> users;
        // principal -> PublicPrincipal JSON, for lookups and fingerprints
        QHash<QString, QJsonObject> directory;
        QHash<QString, QString> registrations;
        QHash<QString, QPair<QString, bool>> logins;
        QHash<QString, QString> mfaTokens;
        QHash<QString, int> mfaAttempts;
        QHash<QString, QString> refreshTokens;
        QHash<QString, Token> accessTokens;
        QHash<QString, Vault> vaults;
        QHash<QString, Blob> blobs;
        QHash<QString, Org> orgs;
        QHash<QString, ServiceAccount> serviceAccounts;
        QHash<QString, SaSecret> secrets;
        QJsonArray audit;
        // Self-hosted sync server: pairing code -> device token
        QHash<QString, QString> pairingCodes;
        QSet<QString> deviceTokens;
        QHash<QString, Blob> databases;
        int counter = 0;
        // Every request, "VERB path" without the API prefix, auth and sync server alike
        QStringList requests;
        // The principal behind each sync server request ("" for device tokens and none)
        QStringList syncPrincipals;
        // The sync server answers PUT vaults/<id> with 412 this many times, as if another device saved first
        int conflictsToInject = 0;

        QString next(const QString& prefix);
        QString issueAccessToken(const QString& principal);
        QJsonObject newSession(const User& user);
        QString roleOf(const QString& vaultId, const QString& principal) const;
        void log(const QString& actor, const QString& action, const QString& target);

        // A user that exists only on the server, e.g. someone to share with; their private keys are
        // returned so tests can act as them
        AccountCrypto::Keyset addUser(const QString& email, QString* principal = nullptr);
        // The user registered (or added) with this email
        QString userId(const QString& email) const;
        QString addOrg(const QString& name, const QString& ownerEmail);
        void addOrgMember(const QString& orgId,
                          const QString& email,
                          const QString& role,
                          const QString& status = QStringLiteral("confirmed"));
        // A pairing code the self-hosted sync server will trade for a device token
        QString addPairingCode();
        // The vault key `principal` was sealed for the vault's current version
        AccountCrypto::SecureBytes vaultKeyFor(const QString& vaultId,
                                               const QString& principal,
                                               const AccountCrypto::SecureBytes& encSk,
                                               int version = -1) const;
    };

    class FakeAuthClient : public AuthClient
    {
    public:
        explicit FakeAuthClient(QSharedPointer<Server> server);

    protected:
        void send(const QByteArray& verb,
                  const QUrl& url,
                  const Headers& headers,
                  const QByteArray& body,
                  int timeoutMsec,
                  Callback callback) override;

    private:
        QSharedPointer<Server> m_server;
    };

    class FakeSyncClient : public SyncServerClient
    {
    public:
        FakeSyncClient(const RemoteParams& params, QSharedPointer<Server> server, QObject* parent = nullptr);

    protected:
        void send(const QByteArray& verb,
                  const QUrl& url,
                  const Headers& headers,
                  const QByteArray& body,
                  int timeoutMsec,
                  Callback callback) override;

    private:
        QSharedPointer<Server> m_server;
    };

    // An account manager talking to `server`, with cheap KDF settings and no event stream
    AccountManager* createManager(const QSharedPointer<Server>& server, QObject* parent = nullptr);
    // SyncServerClient::create() makes FakeSyncClients for `server` until restoreSyncFactory()
    void installSyncFactory(const QSharedPointer<Server>& server);
    void restoreSyncFactory();
} // namespace FakeAccount

#endif // KEEPASSXC_FAKEACCOUNTSERVER_H
