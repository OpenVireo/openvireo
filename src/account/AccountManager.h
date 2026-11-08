/*
 *  Copyright (C) 2026 Bongani Mbigi
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
#include "AccountPlan.h"
#include "AuthClient.h"
#include "OpaqueClient.h"
#include "VaultKeyring.h"

#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncEventStream.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <memory>

class Database;
class QuickUnlockInterface;

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
 *
 * Quick Unlock (Touch ID, Windows Hello, Polkit) works as it does for databases: after the master password
 * unlocks the account once, the AUK is handed to the platform's Quick Unlock provider, which keeps it
 * encrypted in memory under a key the OS only releases after the user authenticates. Locking keeps it, so
 * the next unlock can use quickUnlock() instead of the password; signing out drops it, and like a
 * database's it doesn't survive a restart of the app.
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

    // What /.well-known/openvireo-configuration says about the hosted instance (SPEC §10)
    struct ServerConfig
    {
        QString issuer;
        QString api;
        QString sync;
        // The web app, where account settings live (SPEC §16); empty when the server doesn't say
        QString web;
        // The public share link page (SPEC §22); empty when the server doesn't say
        QString share;
        // OPAQUE idS; the issuer host when the server doesn't say
        QString opaqueServerId;
        quint32 kdfMinMemoryKiB = AccountCrypto::KdfParams::MinMemoryKiB;
        quint32 kdfMinIterations = AccountCrypto::KdfParams::MinIterations;

        bool isValid() const
        {
            return !api.isEmpty();
        }
        QString serverId() const;
        // The first issuer, API, sync or web URL that is neither https:// nor http:// to this computer (SPEC §1)
        QString insecureUrl() const;
        // The web app's Account page (password, two-step verification, devices); invalid without `web`
        QUrl accountSettingsUrl() const;
        // Where share links point: `share`, else `web` + "/s"; empty when neither is a secure URL
        QString shareBase() const;
        static ServerConfig fromJson(const QJsonObject& json);
        QJsonObject toJson() const;
    };

    // An org the account belongs to, from GET /me
    struct Org
    {
        QString id;
        QString name;
        // owner, admin or member
        QString role;
        // invited, accepted or confirmed
        QString status;
        // Who may make share links from the org's vaults: on, managers or off (SPEC §22)
        QString shareLinks = QStringLiteral("on");
        // The org's plan and who bills it (SPEC §19)
        AccountPlan::Plan plan;

        bool isConfirmed() const
        {
            return status.isEmpty() || status == "confirmed";
        }
        bool canAdminister() const
        {
            return isConfirmed() && (role == "owner" || role == "admin");
        }
        bool canShareLinks() const
        {
            return shareLinks == "on" || (shareLinks == "managers" && canAdminister());
        }
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
    // The user's own plan from GET /me (SPEC §19); unknown until it answered
    AccountPlan::Plan plan() const;
    QString deviceId() const;
    QString accessTokenNow() const;
    const AccountCrypto::Keyset& keyset() const;
    const VaultKeyring& keyring() const;
    AuthClient* client() const;
    // The orgs this account belongs to, as of the last reloadVaults(); empty while locked
    QList<Org> orgs() const;
    // Unlocked from this device's offline cache because the servers couldn't be reached (SPEC §20).
    // Reconnecting goes on in the background until GET /vaults answers again.
    bool isOffline() const;
    // This device can unlock the account without a connection
    bool hasOfflineCache() const;
    Org org(const QString& orgId) const;

    // The Quick Unlock provider; tests set a mock, the default is the platform's (getQuickUnlock())
    void setQuickUnlock(QuickUnlockInterface* quickUnlock);
    // Quick Unlock is on in the settings and this platform has a provider
    bool isQuickUnlockAvailable() const;
    // The account is locked and its AUK is held by the Quick Unlock provider
    bool canQuickUnlock() const;

    // Hands out a fresh access token, refreshing it first when it is about to expire. Thread safe.
    void accessToken(std::function<void(const QString& token)> done);
    AccessTokenProvider tokenProvider();
    // The remote settings that sync a vault of this account
    RemoteParams vaultRemoteParams(const QString& vaultId) const;

    // The Secret Key this device keeps for an account, empty when it doesn't trust the device yet
    static QString storedSecretKey(const QString& serverId, const QString& email);
    // The account this device last signed in to, for restoreSession()
    static QString lastAccountId();

    // An account this device knows (SPEC §21). One is open at a time; the others can stay signed in.
    struct KnownAccount
    {
        QString id;
        QString email;
        ServerConfig server;
        qint64 lastUsedAt = 0;
        // When a session set aside last heard from its server
        qint64 checkedAt = 0;
        // It has a session on this device
        bool signedIn = false;
        // It's the one open now
        bool current = false;
        // The erase date (RFC 3339) while it is scheduled for deletion (SPEC §23)
        QString deletionScheduledFor;
    };
    // Most recently used first
    QList<KnownAccount> knownAccounts() const;

    using CreateVaultCallback =
        std::function<void(const QString& vaultId, QSharedPointer<Database> db, const QString& error)>;
    // Create a personal vault: its key is sealed to this account. `done` gets the empty vault database
    // to save and upload with If-None-Match: *, or an error.
    void createVault(const QString& name, CreateVaultCallback done);

    // Account deletion (SPEC §23). The erase date while the signed-in account is pending: it opens no
    // vaults until restoreAccount().
    QString deletionScheduledFor() const;
    // What deleting will do (GET /me/deletion), passed through as the server's JSON
    void deletionPreview(std::function<void(const QJsonObject& preview, const QString& error)> done);
    // Checks the master password, schedules the deletion and signs this device out (its offline cache
    // goes; the Secret Key stays, so the account can still be restored). `done` gets the erase date.
    void deleteAccount(const QString& password,
                       const QString& confirm,
                       std::function<void(const QString& scheduledFor, const QString& error)> done);
    void restoreAccount(const QString& password, std::function<void(const QString& error)> done);
    void transferVault(const QString& vaultId, const QString& principal, std::function<void(const QString& error)> done);
    // Drops remembered accounts (and their Secret Keys) a day past their erase date
    void removeErasedAccounts();

public slots:
    // Create an account. registered() hands out the Secret Key for the Emergency Kit.
    void registerAccount(const QString& email, const QString& password);
    // `secretKey` is needed on a device that doesn't keep the account's Secret Key yet
    void signIn(const QString& email, const QString& password, const QString& secretKey = {});
    void submitMfa(const QString& totp);
    // Sign in with a service account token (ov_sa_...)
    void signInServiceAccount(const QString& token);
    // After a restart: pick the session up from the keychain, Locked until unlock()
    bool restoreSession(const QString& accountId);
    // Locks the account and leaves its session in the keychain for its user to come back to: the server isn't told
    // and the offline cache and Quick Unlock stay (SPEC §21). A refresh under way finishes first.
    void setAside(std::function<void()> done);
    // Sets the open account aside and opens another, Locked. False when that account has no session: it signs in.
    void switchTo(const QString& accountId, std::function<void(bool ok)> done);
    // Signs out of an account that isn't open: it stays known, signed out
    void signOutKnown(const QString& accountId);
    // Removes an account that isn't open from this device, with its Secret Key and device id
    void forgetKnown(const QString& accountId);
    // Asks the servers about sessions set aside, each at most once a day: this keeps them within their 30 days, and
    // a revoked device finds out (SPEC §21)
    void checkSetAsideAccounts();
    void unlock(const QString& password);
    // Unlock with the AUK the Quick Unlock provider holds; it asks the user to authenticate first
    void quickUnlock();
    // Wipe the keyset and the vault keys from memory; the session stays
    void lock();
    // End the session on the server and forget it here; the Secret Key stays on the device
    void signOut();
    void refreshAccessToken();
    void reloadVaults();
    // While offline: try the servers again now instead of waiting for the next retry
    void reconnect();
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
    void knownAccountsChanged();
    void accessTokenChanged();
    void vaultsChanged();
    void orgsChanged();
    // The account's email changed, e.g. in the web app (SPEC §16)
    void emailChanged();
    void offlineChanged(bool offline);
    // A vault's blob changed on the sync server
    void vaultChanged(const SyncEventStream::Event& event);
    void error(const QString& message);
    // The signed-in account turned out to be pending deletion, or was restored
    void pendingDeletionChanged();

private:
    struct Pending;

    void setState(State state);
    void fail(const QString& message);
    void finishLogin(const QJsonObject& session);
    void deriveKeyset(const QJsonObject& kdfJson,
                      const QJsonObject& wrappedKeyset,
                      std::function<void(bool ok, const QString& error)> done);
    void finishUnlock(bool offline);
    void setOffline(bool offline);
    void scheduleReconnect();
    // GET /me's kdf, keyset and orgs into the offline cache, and the orgs into orgs()
    void takeProfile(const QJsonObject& me);
    QString unreachableMessage(const AuthClient::Response& response) const;
    void followEmail(const QString& email);
    void reloadOrgs();
    QUuid quickUnlockId() const;
    void rememberAuk(const SecureBytes& auk);
    void forgetAuk();
    void setTokens(const QString& accessToken, const QString& refreshToken, int expiresIn);
    void exchangeServiceAccount();
    void finishRefresh(bool ok);
    void endSession(const QString& reason);
    void saveSession() const;
    void startEvents();
    void stopEvents();
    QString defaultServerId() const;
    OpaqueClient::Ids opaqueIds(const QString& email) const;

    void rememberCurrentAccount() const;
    // Runs `done` once no refresh is rotating a token this device holds
    void whenTokensSettled(std::function<void()> done);
    void finishSetAsideCheck();
    void setPendingDeletion(const QString& scheduledFor);
    void setKnownDeletion(const QString& accountId, const QString& scheduledFor) const;
    void verifyPassword(const QString& password, std::function<void(const QString& error)> done);
    void freshToken(std::function<void(bool ok)> done);

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
    int m_setAsideChecks = 0;
    QList<std::function<void()>> m_setAsideCheckWaiters;

    std::unique_ptr<Pending> m_pending;
    AccountCrypto::Keyset m_keyset;
    VaultKeyring m_keyring;
    QList<Org> m_orgs;
    AccountPlan::Plan m_plan;
    QuickUnlockInterface* m_quickUnlock = nullptr;
    QPointer<SyncEventStream> m_events;

    bool m_offline = false;
    QString m_deletionScheduledFor;
    QTimer m_reconnectTimer;
    int m_reconnectDelaySec = 0;
};

#endif // KEEPASSXC_ACCOUNTMANAGER_H
