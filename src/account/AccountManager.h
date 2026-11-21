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
#include "CoreHost.h"
#include "VaultKeyring.h"

#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncEventStream.h"
#include "gui/remote/SyncServerClient.h"

#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <memory>

class CompositeKey;
class Database;
class QuickUnlockInterface;

/**
 * One OpenVireo account on this device (docs/plans/accounts/SPEC.md). The session itself (sign-up and sign-in
 * with OPAQUE and the Secret Key, unlock, the tokens and their refresh, sessions set aside) runs in
 * openvireo-core's ov::Account, on a worker thread; this class keeps the Qt API and signals around it.
 *
 *   SignedOut ──signIn()──▶ SigningIn ──▶ [NeedsMfa ──submitMfa()──▶] Unlocked ──lock()──▶ Locked
 *       ▲                                                               │  ▲                 │
 *       └──────────── signOut(), or the session was revoked ────────────┘  └────unlock()─────┘
 *
 * The keyset (and with it every vault key) is only in memory while Unlocked and is wiped by lock().
 * The core keeps the refresh token and the Secret Key in the device's keychain (CoreHost::SecretStore), so after
 * a restart restoreSession() gets back to Locked and unlock() only needs the master password.
 *
 * Access tokens live 10 minutes. The core refreshes them a minute before they expire, and this manager asks it to
 * whenever the sync server announces an access change (SPEC §11). It is the only refresher: everything else
 * borrows the core's token through accessToken(). Presenting a rotated refresh token makes the server revoke
 * the session, which signs this device out.
 *
 * Account vaults are kept as KDBX files (KeePassXC Databases) under the vault key as a raw key file. The core
 * lists the vaults, hands out each one's key (Account::vault_kdbx_credentials), downloads and creates them, and
 * syncs the files (Account::sync_vault_kdbx): this manager registers itself with SyncServerClient as its account's
 * token provider and vault syncer while signed in, and SyncEngine syncs through it.
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
    // For tests: the core's Host (transport, clock, keychain, cache) from `host` instead of the real ones
    explicit AccountManager(CoreHost::Factory host, QObject* parent = nullptr);
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
    // Create a vault: its key is sealed to this account (and in an org to its owners and admins), and the core
    // uploads it empty with If-None-Match: *. `done` gets the vault database to save as the local copy, or an error.
    void createVault(const QString& name, CreateVaultCallback done);
    void createVault(const QString& name, const QString& orgId, CreateVaultCallback done);
    using DownloadVaultCallback =
        std::function<void(QSharedPointer<Database> db, const QString& errorCode, const QString& error)>;
    // Download a vault this device has no copy of yet; `db` is under the vault's newest key. `errorCode` is the
    // core's wire code: not_found when nothing was uploaded yet.
    void downloadVault(const QString& vaultId, DownloadVaultCallback done);
    // Sync a vault file in the core (SyncEngine calls this through SyncServerClient::accountVaultSyncerFor())
    AccountVaultSync::Cancel syncVault(const AccountVaultSync::Request& request, AccountVaultSync::Done done);
    // The composite key of a vault's newest key version: the vault key as a raw key file; null without it
    QSharedPointer<CompositeKey> vaultKey(const QString& vaultId) const;
    // A vault key (a raw 32-byte key file) as a KeePassXC composite key
    static QSharedPointer<CompositeKey> vaultCompositeKey(const QByteArray& keyFile);

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

public:
    // What a worker reads from the core after a sign-in or unlock
    struct SessionInfo;
    // A failed core call (ov::Error), as plain Qt data
    struct CoreError
    {
        QString code;
        QString message;
        QString details;
        // The server's own code when it isn't a wire code (account_pending_deletion, ...)
        QString serverCode;
        int status = 0;
        bool ok() const
        {
            return code.isEmpty();
        }
    };

    // The vault list as a worker read it from the core
    struct VaultList;

    // A call on the signed-in account's openvireo-core session; it runs on the core's worker thread
    using AccountWork = std::function<void(const ov::Account& account, const ov::CancelToken& cancel)>;
    using AccountDone = std::function<void(const CoreError& error)>;
    // Runs `work` with the unlocked account, then `done` on this thread unless `context` is gone; a session the
    // core ended (refused refresh) signs this device out. signed_out without an unlocked account.
    void callAccount(QObject* context, AccountWork work, AccountDone done);
    // The message to show for a failed core call
    QString errorMessage(const CoreError& error) const;

private:
    struct Pending;
    struct CoreSession;
    struct EventLoop;
    enum class CoreEnd
    {
        // The core ended the session already (refused refresh)
        None,
        // Sign out: the server is told, the offline data goes
        SignOut,
        // As SignOut, and the Secret Key and device id go too
        Forget
    };

    void setState(State state);
    void fail(const QString& message);
    void finishSignIn(const SessionInfo& info, bool registering);
    void finishUnlock(bool offline);
    void setOffline(bool offline);
    void scheduleReconnect();
    // GET /me's kdf, keyset and orgs into the offline cache, and the orgs into orgs()
    void takeProfile(const QJsonObject& me);
    QString unreachableMessage() const;
    QString messageFor(const CoreError& error) const;
    // A refused refresh ended the session in the core: end it here too. True when it did.
    bool sessionEndedBy(const CoreError& error);
    void followEmail(const QString& email);
    void reloadOrgs();
    QUuid quickUnlockId() const;
    void rememberAuk(const SecureBytes& auk);
    void forgetAuk();
    void takeToken(const QString& token);
    void finishTokenRequest(const CoreError& error);
    void exchangeServiceAccount();
    void finishRefresh(bool ok);
    void endSession(const QString& reason, CoreEnd end = CoreEnd::None);
    void startEvents();
    void stopEvents();
    void takeCoreEvent(const ov::AccountEvent& event);
    void takeVaultList(const VaultList& list);
    void registerAccountHooks();
    void unregisterAccountHooks();
    QString defaultServerId() const;

    // The core client and account for `config`, without network; null when the config isn't usable
    std::shared_ptr<CoreSession> makeCore(const ServerConfig& config) const;
    // Runs `work` on this manager's worker (one at a time, in order), then `done` on this thread
    void runCore(std::shared_ptr<CoreSession> core,
                 std::function<void(CoreSession&)> work,
                 std::function<void(const CoreError&)> done);
    void rememberCurrentAccount() const;
    void setPendingDeletion(const QString& scheduledFor);
    void setKnownDeletion(const QString& accountId, const QString& scheduledFor) const;

    CoreHost::Factory m_host;
    // Session-state queries that need no account (SPEC §21)
    std::shared_ptr<CoreSession> m_local;
    std::shared_ptr<CoreSession> m_core;
    QThreadPool m_pool;
    int m_inFlight = 0;
    bool m_destroying = false;
    AuthClient* m_client;
    ServerConfig m_config;
    AccountCrypto::KdfParams m_newAccountKdf;
    bool m_eventsEnabled = true;

    State m_state = State::SignedOut;
    QString m_accountId;
    QString m_email;
    QString m_deviceId;
    bool m_serviceAccount = false;
    AccountCrypto::ServiceAccountToken m_saToken;

    // The last token the core handed out, and when it expires on this device's clock
    QString m_accessToken;
    qint64 m_accessTokenExpiry = 0;
    QTimer m_refreshTimer;
    bool m_refreshing = false;
    bool m_reloadAfterRefresh = false;
    QList<std::function<void(const QString&)>> m_tokenWaiters;

    std::unique_ptr<Pending> m_pending;
    AccountCrypto::Keyset m_keyset;
    VaultKeyring m_keyring;
    QList<Org> m_orgs;
    AccountPlan::Plan m_plan;
    QuickUnlockInterface* m_quickUnlock = nullptr;
    // The core's event stream (Account::subscribe), read on a thread of its own
    std::shared_ptr<EventLoop> m_events;

    bool m_offline = false;
    QString m_deletionScheduledFor;
    QTimer m_reconnectTimer;
    int m_reconnectDelaySec = 0;
};

#endif // KEEPASSXC_ACCOUNTMANAGER_H
