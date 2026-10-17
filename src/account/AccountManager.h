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

#ifndef KEEPASSXC_ACCOUNTMANAGER_H
#define KEEPASSXC_ACCOUNTMANAGER_H

#include "AccountCrypto.h"
#include "AuthClient.h"
#include "OpaqueClient.h"
#include "VaultKeyring.h"

#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncEventStream.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <memory>

class Database;

/**
 * One KeePassXC account on this device (docs/plans/accounts/SPEC.md): signing up and in with OPAQUE and
 * the Secret Key, the access token and its refresh, the account keyset and the vault keys.
 *
 *   SignedOut ──signIn()──▶ SigningIn ──▶ [NeedsMfa ──submitMfa()──▶] Unlocked ──lock()──▶ Locked
 *       ▲                                                               │  ▲                 │
 *       └──────────── signOut(), or the session was revoked ────────────┘  └────unlock()─────┘
 *
 * The keyset (and with it every vault key) is only in memory while Unlocked and is wiped by lock().
 * The refresh token and the Secret Key stay in the device's keychain (SyncTokenStore), so after a
 * restart restoreSession() gets back to Locked and unlock() only needs the master password.
 *
 * Access tokens live 10 minutes. They are refreshed a minute before they expire, when a request finds
 * one that is about to, and whenever the sync server announces an access change (SPEC §11). Presenting
 * a rotated refresh token makes the server revoke the session, which signs this device out.
 *
 * Account vaults get their tokens from this manager: it registers itself as the token provider of
 * its account with SyncServerClient while signed in.
 */
class AccountManager : public QObject
{
    Q_OBJECT

public:
    using SecureBytes = AccountCrypto::SecureBytes;

    enum class State
    {
        SignedOut,
        // OPAQUE and the key derivation are running
        SigningIn,
        // The password was right, the server wants a TOTP code: submitMfa()
        NeedsMfa,
        // Signed in, but the keyset isn't in memory: unlock()
        Locked,
        Unlocked
    };
    Q_ENUM(State)

    // What /.well-known/kpx-configuration says about the hosted instance (SPEC §10)
    struct ServerConfig
    {
        QString issuer;
        QString api;
        QString sync;
        // OPAQUE idS; the issuer host when the server doesn't say
        QString opaqueServerId;
        quint32 kdfMinMemoryKiB = AccountCrypto::KdfParams::MinMemoryKiB;
        quint32 kdfMinIterations = AccountCrypto::KdfParams::MinIterations;

        bool isValid() const
        {
            return !api.isEmpty();
        }
        QString serverId() const;
        static ServerConfig fromJson(const QJsonObject& json);
        QJsonObject toJson() const;
    };

    // Refresh this long before the access token expires
    static constexpr int RefreshMarginSec = 60;
    // A token that expires sooner than this isn't handed out without refreshing first
    static constexpr int MinTokenLifetimeSec = 30;

    explicit AccountManager(QObject* parent = nullptr);
    // For tests: a client that doesn't use the network and another OPAQUE implementation
    AccountManager(AuthClient* client, std::unique_ptr<OpaqueClient> opaque, QObject* parent = nullptr);
    ~AccountManager() override;

    void setServer(const ServerConfig& config);
    ServerConfig server() const;
    // Fetch the server's configuration from its issuer URL; serverChanged() or error() follows
    void discover(const QString& issuerUrl);

    // The KDF settings new accounts get; the production defaults unless tests lower them
    void setNewAccountKdf(const AccountCrypto::KdfParams& kdf);
    // Whether this manager's own event stream (for `access` events) connects; tests turn it off
    void setEventStreamEnabled(bool enabled);

    State state() const;
    bool isServiceAccount() const;
    // The user id, or the service account id
    QString accountId() const;
    // user:<id> or sa:<id>
    QString principal() const;
    QString email() const;
    QString deviceId() const;
    QString accessTokenNow() const;
    const AccountCrypto::Keyset& keyset() const;
    const VaultKeyring& keyring() const;
    AuthClient* client() const;

    // Hands out a fresh access token, refreshing it first when it is about to expire. Thread safe.
    void accessToken(std::function<void(const QString& token)> done);
    AccessTokenProvider tokenProvider();
    // The remote settings that sync a vault of this account
    RemoteParams vaultRemoteParams(const QString& vaultId) const;

    // The Secret Key this device keeps for an account, empty when it doesn't trust the device yet
    static QString storedSecretKey(const QString& serverId, const QString& email);
    // The account this device last signed in to, for restoreSession()
    static QString lastAccountId();

    using CreateVaultCallback =
        std::function<void(const QString& vaultId, QSharedPointer<Database> db, const QString& error)>;
    // Create a personal vault: its key is sealed to this account. `done` gets the empty vault database
    // to save and upload with If-None-Match: *, or an error.
    void createVault(const QString& name, CreateVaultCallback done);

public slots:
    // Create an account. registered() hands out the Secret Key for the Emergency Kit.
    void registerAccount(const QString& email, const QString& password);
    // `secretKey` is needed on a device that doesn't keep the account's Secret Key yet
    void signIn(const QString& email, const QString& password, const QString& secretKey = {});
    void submitMfa(const QString& totp);
    // Sign in with a service account token (kpx_sa_...)
    void signInServiceAccount(const QString& token);
    // After a restart: pick the session up from the keychain, Locked until unlock()
    bool restoreSession(const QString& accountId);
    void unlock(const QString& password);
    // Wipe the keyset and the vault keys from memory; the session stays
    void lock();
    // End the session on the server and forget it here; the Secret Key stays on the device
    void signOut();
    void refreshAccessToken();
    void reloadVaults();
    // An `access` event from the sync server: refresh the token for its new claims, reload the vaults
    // and reconnect the event stream
    void handleAccessEvent(const SyncEventStream::Event& event);

signals:
    void stateChanged(AccountManager::State state);
    void serverChanged();
    void mfaRequired(const QStringList& methods);
    // The new account's Secret Key, to show once and print in the Emergency Kit
    void registered(const QString& secretKey);
    void signedIn();
    // The session ended: signed out here, revoked on the server or the refresh token was reused
    void signedOut(const QString& reason);
    void accessTokenChanged();
    void vaultsChanged();
    // A vault's blob changed on the sync server
    void vaultChanged(const SyncEventStream::Event& event);
    void error(const QString& message);

private:
    struct Pending;

    void setState(State state);
    void fail(const QString& message);
    void finishLogin(const QJsonObject& session);
    void deriveKeyset(const QJsonObject& kdfJson,
                      const QJsonObject& wrappedKeyset,
                      std::function<void(bool ok, const QString& error)> done);
    void setTokens(const QString& accessToken, const QString& refreshToken, int expiresIn);
    void exchangeServiceAccount();
    void finishRefresh(bool ok);
    void endSession(const QString& reason);
    void saveSession() const;
    void startEvents();
    void stopEvents();
    QString defaultServerId() const;
    OpaqueClient::Ids opaqueIds(const QString& email) const;

    static QString sessionKey(const QString& accountId);
    static QString secretKeyKey(const QString& serverId, const QString& email);
    static QString deviceKey(const QString& serverId, const QString& email);

    AuthClient* m_client;
    std::unique_ptr<OpaqueClient> m_opaque;
    ServerConfig m_config;
    AccountCrypto::KdfParams m_newAccountKdf;
    bool m_eventsEnabled = true;

    State m_state = State::SignedOut;
    QString m_accountId;
    QString m_email;
    QString m_deviceId;
    bool m_serviceAccount = false;
    AccountCrypto::ServiceAccountToken m_saToken;

    QString m_accessToken;
    QString m_refreshToken;
    qint64 m_accessTokenExpiry = 0;
    QTimer m_refreshTimer;
    bool m_refreshing = false;
    bool m_reloadAfterRefresh = false;
    QList<std::function<void(const QString&)>> m_tokenWaiters;

    std::unique_ptr<Pending> m_pending;
    AccountCrypto::Keyset m_keyset;
    VaultKeyring m_keyring;
    QPointer<SyncEventStream> m_events;
};

#endif // KEEPASSXC_ACCOUNTMANAGER_H
