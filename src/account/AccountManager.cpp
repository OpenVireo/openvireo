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

#include "AccountManager.h"

#include "account/AccountOfflineCache.h"
#include "core/AsyncTask.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Tools.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/remote/SyncTokenStore.h"
#include "quickunlock/QuickUnlockInterface.h"

#include <botan/mem_ops.h>

#include <algorithm>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkInformation>
#endif
#include <QThread>
#include <QUrl>
#include <QUuid>

using namespace AccountCrypto;

// What a sign-in or registration in progress keeps until the session arrives
struct AccountManager::Pending
{
    bool registering = false;
    QString email;
    SecureBytes password;
    SecureBytes secretKey;
    SecureBytes opaqueSecret;
    QString sessionId;
    QString mfaToken;
    // Registration only
    KdfParams kdf;
    QString userId;
};

namespace
{
    const QString LastAccountKey = QStringLiteral("ov-account-last");
    const QString KnownAccountsKey = QStringLiteral("ov-account-known");
    constexpr qint64 SetAsideCheckIntervalSec = 24 * 60 * 60;

    qint64 now()
    {
        return QDateTime::currentSecsSinceEpoch();
    }

    QString hostOf(const QString& url)
    {
        return QUrl(url).host();
    }

    // Quick Unlock entries of accounts live beside those of databases, keyed by a name-based uuid
    const QUuid QuickUnlockNamespace("8c0d3f4e-5a6b-4c7d-9e8f-0a1b2c3d4e5f");

    QUuid quickUnlockIdOf(const QString& serverId, const QString& accountId)
    {
        return QUuid::createUuidV5(QuickUnlockNamespace, "ov-account:" + serverId + ":" + accountId);
    }

    // While offline the servers are tried again after this long, doubling up to the maximum
    constexpr int FirstReconnectDelaySec = 15;
    constexpr int MaxReconnectDelaySec = 5 * 60;

    QString insecureServerMessage(const QString& url)
    {
        return AccountManager::tr("The server sent an address that doesn't use https://: %1. Plain http:// is only "
                                  "allowed for this computer.")
            .arg(url);
    }

    QList<AccountManager::Org> parseOrgs(const QJsonArray& array)
    {
        QList<AccountManager::Org> orgs;
        for (const auto& value : array) {
            const auto json = value.toObject();
            AccountManager::Org org{json.value("id").toString(),
                                    json.value("name").toString(),
                                    json.value("role").toString(),
                                    json.value("status").toString()};
            org.shareLinks = json.value("item_share_links").toString("on");
            orgs << org;
        }
        return orgs;
    }
} // namespace

QString AccountManager::ServerConfig::serverId() const
{
    return opaqueServerId.isEmpty() ? hostOf(issuer.isEmpty() ? api : issuer) : opaqueServerId;
}

QUrl AccountManager::ServerConfig::accountSettingsUrl() const
{
    QUrl url(web);
    if (web.isEmpty() || !Tools::isSecureServerUrl(url)) {
        return {};
    }
    if (url.path().isEmpty()) {
        url.setPath("/");
    }
    url.setQuery("view=account");
    return url;
}

QString AccountManager::ServerConfig::shareBase() const
{
    auto base = share.isEmpty() && !web.isEmpty() ? web + "/s" : share;
    while (base.endsWith('/')) {
        base.chop(1);
    }
    return base.isEmpty() || !Tools::isSecureServerUrl(QUrl(base)) ? QString() : base;
}

QString AccountManager::ServerConfig::insecureUrl() const
{
    for (const auto& url : {issuer, api, sync, web, share}) {
        if (!url.isEmpty() && !Tools::isSecureServerUrl(QUrl(url))) {
            return url;
        }
    }
    return {};
}

AccountManager::ServerConfig AccountManager::ServerConfig::fromJson(const QJsonObject& json)
{
    ServerConfig config;
    config.issuer = json.value("issuer").toString();
    config.api = json.value("api").toString();
    config.sync = json.value("sync").toString();
    config.web = json.value("web").toString();
    config.share = json.value("share").toString();
    const auto opaque = json.value("opaque").toObject();
    config.opaqueServerId = opaque.value("ids").toString();
    const auto kdfMin = json.value("kdf_min").toObject();
    // A server can only raise the minimum, never lower what clients accept
    config.kdfMinMemoryKiB = qMax<quint32>(KdfParams::MinMemoryKiB, kdfMin.value("m").toInt());
    config.kdfMinIterations = qMax<quint32>(KdfParams::MinIterations, kdfMin.value("t").toInt());
    return config;
}

QJsonObject AccountManager::ServerConfig::toJson() const
{
    return QJsonObject{
        {"issuer", issuer},
        {"api", api},
        {"sync", sync},
        {"web", web},
        {"share", share},
        {"opaque", QJsonObject{{"ids", opaqueServerId}}},
        {"kdf_min",
         QJsonObject{{"m", static_cast<qint64>(kdfMinMemoryKiB)}, {"t", static_cast<qint64>(kdfMinIterations)}}}};
}

AccountManager::AccountManager(QObject* parent)
    : AccountManager(new AuthClient(), std::make_unique<OpaqueClient>(), parent)
{
}

AccountManager::AccountManager(AuthClient* client, std::unique_ptr<OpaqueClient> opaque, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_opaque(std::move(opaque))
{
    m_client->setParent(this);
    m_client->setTokenProvider(tokenProvider());
    m_refreshTimer.setSingleShot(true);
    connect(&m_refreshTimer, &QTimer::timeout, this, &AccountManager::refreshAccessToken);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &AccountManager::reconnect);

#ifdef KPXC_FEATURE_NETWORK
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability);
#else
    QNetworkInformation::load(QNetworkInformation::Feature::Reachability);
#endif
    if (auto* network = QNetworkInformation::instance()) {
        connect(network,
                &QNetworkInformation::reachabilityChanged,
                this,
                [this](QNetworkInformation::Reachability reachability) {
                    if (reachability == QNetworkInformation::Reachability::Online && m_offline) {
                        reconnect();
                    }
                });
    }
#endif
}

AccountManager::~AccountManager()
{
    if (!m_accountId.isEmpty()) {
        SyncServerClient::setAccountTokenProvider(m_accountId, nullptr);
    }
    m_keyring.clear();
    m_keyset.clear();
}

void AccountManager::setServer(const ServerConfig& config)
{
    m_config = config;
    m_client->setApiUrl(config.api);
    emit serverChanged();
}

AccountManager::ServerConfig AccountManager::server() const
{
    return m_config;
}

void AccountManager::discover(const QString& issuerUrl)
{
    m_client->discover(issuerUrl, [this](const AuthClient::Response& response) {
        const auto config = ServerConfig::fromJson(response.json());
        if (!response.isSuccess() || !config.isValid()) {
            emit error(response.isSuccess() ? tr("The server isn't an OpenVireo account server.")
                                            : AuthClient::errorMessage(response));
            return;
        }
        if (const auto insecure = config.insecureUrl(); !insecure.isEmpty()) {
            emit error(insecureServerMessage(insecure));
            return;
        }
        setServer(config);
    });
}

void AccountManager::setNewAccountKdf(const KdfParams& kdf)
{
    m_newAccountKdf = kdf;
}

void AccountManager::setEventStreamEnabled(bool enabled)
{
    m_eventsEnabled = enabled;
    if (!enabled) {
        stopEvents();
    }
}

AccountManager::State AccountManager::state() const
{
    return m_state;
}

bool AccountManager::isServiceAccount() const
{
    return m_serviceAccount;
}

QString AccountManager::accountId() const
{
    return m_accountId;
}

QString AccountManager::principal() const
{
    if (m_accountId.isEmpty()) {
        return {};
    }
    return (m_serviceAccount ? "sa:" : "user:") + m_accountId;
}

QString AccountManager::email() const
{
    return m_email;
}

QString AccountManager::deviceId() const
{
    return m_deviceId;
}

QString AccountManager::accessTokenNow() const
{
    return m_accessToken;
}

const Keyset& AccountManager::keyset() const
{
    return m_keyset;
}

const VaultKeyring& AccountManager::keyring() const
{
    return m_keyring;
}

AuthClient* AccountManager::client() const
{
    return m_client;
}

QList<AccountManager::Org> AccountManager::orgs() const
{
    return m_orgs;
}

bool AccountManager::isOffline() const
{
    return m_offline;
}

bool AccountManager::hasOfflineCache() const
{
    return !m_serviceAccount && AccountOfflineCache::hasProfile(m_accountId);
}

AccountManager::Org AccountManager::org(const QString& orgId) const
{
    for (const auto& org : m_orgs) {
        if (org.id == orgId) {
            return org;
        }
    }
    return {};
}

void AccountManager::setQuickUnlock(QuickUnlockInterface* quickUnlock)
{
    m_quickUnlock = quickUnlock;
}

bool AccountManager::isQuickUnlockAvailable() const
{
    // The same switch as the databases' Quick Unlock (Settings, Security)
    if (m_serviceAccount || !config()->get(Config::Security_QuickUnlock).toBool()) {
        return false;
    }
    auto* provider = m_quickUnlock ? m_quickUnlock : getQuickUnlock();
    return provider->isAvailable();
}

bool AccountManager::canQuickUnlock() const
{
    if (m_state != State::Locked || m_accountId.isEmpty() || !isQuickUnlockAvailable()) {
        return false;
    }
    auto* provider = m_quickUnlock ? m_quickUnlock : getQuickUnlock();
    return provider->hasKey(quickUnlockId());
}

QUuid AccountManager::quickUnlockId() const
{
    return quickUnlockIdOf(defaultServerId(), m_accountId);
}

void AccountManager::rememberAuk(const SecureBytes& auk)
{
    if (!isQuickUnlockAvailable() || m_accountId.isEmpty()) {
        return;
    }
    // The provider encrypts it right away and keeps it in memory only, as it does a database key
    auto key = fromSecure(auk);
    auto* provider = m_quickUnlock ? m_quickUnlock : getQuickUnlock();
    provider->setKey(quickUnlockId(), key);
    Botan::secure_scrub_memory(key.data(), key.size());
}

void AccountManager::forgetAuk()
{
    if (m_accountId.isEmpty()) {
        return;
    }
    auto* provider = m_quickUnlock ? m_quickUnlock : getQuickUnlock();
    provider->reset(quickUnlockId());
}

void AccountManager::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged(state);
    }
}

void AccountManager::fail(const QString& message)
{
    // Whatever was in progress is over; an existing session stays
    m_pending.reset();
    if (m_state == State::SigningIn || m_state == State::NeedsMfa) {
        setState(m_refreshToken.isEmpty() && m_accessToken.isEmpty() ? State::SignedOut : State::Locked);
    }
    emit error(message);
}

QString AccountManager::defaultServerId() const
{
    return m_config.serverId();
}

OpaqueClient::Ids AccountManager::opaqueIds(const QString& email) const
{
    return {OpaqueClient::userId(email), defaultServerId().toUtf8()};
}

QString AccountManager::sessionKey(const QString& accountId)
{
    return "ov-account-session:" + accountId;
}

QString AccountManager::secretKeyKey(const QString& serverId, const QString& email)
{
    return "ov-account-secret-key:" + serverId + ":" + email.trimmed().toLower();
}

QString AccountManager::deviceKey(const QString& serverId, const QString& email)
{
    return "ov-account-device:" + serverId + ":" + email.trimmed().toLower();
}

QString AccountManager::storedSecretKey(const QString& serverId, const QString& email)
{
    return SyncTokenStore::load(secretKeyKey(serverId, email));
}

QString AccountManager::lastAccountId()
{
    return SyncTokenStore::load(LastAccountKey);
}

QList<AccountManager::KnownAccount> AccountManager::knownAccounts() const
{
    QList<KnownAccount> list;
    const auto array = QJsonDocument::fromJson(SyncTokenStore::load(KnownAccountsKey).toUtf8()).array();
    for (const auto& value : array) {
        const auto json = value.toObject();
        KnownAccount account;
        account.id = json.value("id").toString();
        account.email = json.value("email").toString();
        account.server = ServerConfig::fromJson(json.value("server").toObject());
        account.lastUsedAt = json.value("last_used_at").toInteger();
        account.checkedAt = json.value("checked_at").toInteger();
        account.deletionScheduledFor = json.value("deletion_scheduled_for").toString();
        account.signedIn = !SyncTokenStore::load(sessionKey(account.id)).isEmpty();
        account.current = account.id == m_accountId && !m_serviceAccount;
        if (!account.id.isEmpty()) {
            list << account;
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const KnownAccount& a, const KnownAccount& b) {
        return a.lastUsedAt > b.lastUsedAt;
    });
    return list;
}

namespace
{
    void saveKnownAccounts(const QList<AccountManager::KnownAccount>& list)
    {
        QJsonArray array;
        for (const auto& account : list) {
            QJsonObject json{{"id", account.id},
                             {"email", account.email},
                             {"server", account.server.toJson()},
                             {"last_used_at", account.lastUsedAt},
                             {"checked_at", account.checkedAt}};
            if (!account.deletionScheduledFor.isEmpty()) {
                json.insert("deletion_scheduled_for", account.deletionScheduledFor);
            }
            array << json;
        }
        SyncTokenStore::save(KnownAccountsKey, QJsonDocument(array).toJson(QJsonDocument::Compact));
    }
} // namespace

void AccountManager::rememberCurrentAccount() const
{
    if (m_serviceAccount || m_accountId.isEmpty()) {
        return;
    }
    auto list = knownAccounts();
    qint64 checkedAt = 0;
    QString deletion;
    list.removeIf([&](const KnownAccount& account) {
        if (account.id != m_accountId) {
            return false;
        }
        checkedAt = account.checkedAt;
        deletion = account.deletionScheduledFor;
        return true;
    });
    KnownAccount current;
    current.id = m_accountId;
    current.email = m_email;
    current.server = m_config;
    current.lastUsedAt = now();
    current.checkedAt = checkedAt;
    current.deletionScheduledFor = deletion;
    list.prepend(current);
    saveKnownAccounts(list);
}

void AccountManager::registerAccount(const QString& email, const QString& password)
{
    if (!m_config.isValid()) {
        emit error(tr("Choose an account server first."));
        return;
    }
    m_pending = std::make_unique<Pending>();
    m_pending->registering = true;
    m_pending->email = email.trimmed();
    m_pending->password = normalizePassword(password);
    m_pending->secretKey = generateSecretKey();
    m_pending->kdf = m_newAccountKdf.salt.isEmpty() ? KdfParams::generate() : m_newAccountKdf;
    if (m_pending->kdf.salt.isEmpty()) {
        m_pending->kdf.salt = KdfParams::generate().salt;
    }
    setState(State::SigningIn);

    QByteArray request;
    if (!m_opaque->createRegistrationRequest(m_pending->password, &m_pending->opaqueSecret, &request)) {
        fail(OpaqueClient::isAvailable() ? tr("Could not start the registration.")
                                         : tr("This build of OpenVireo can't sign in to accounts."));
        return;
    }
    m_client->registerStart(m_pending->email, request, [this](const AuthClient::Response& response) {
        if (!m_pending) {
            return;
        }
        if (!response.isSuccess()) {
            fail(response.status == 409 ? tr("An account with this email address already exists.")
                                        : AuthClient::errorMessage(response));
            return;
        }
        const auto json = response.json();
        m_pending->sessionId = json.value("session_id").toString();
        m_pending->userId = json.value("user_id").toString();
        QByteArray record;
        if (!m_opaque->finalizeRegistration(m_pending->opaqueSecret,
                                            fromBase64Url(json.value("response").toString()),
                                            opaqueIds(m_pending->email),
                                            &record)) {
            fail(tr("The account server sent an invalid registration response."));
            return;
        }

        // The keyset is created here and only ever leaves the device wrapped with the AUK
        const auto kdf = m_pending->kdf;
        const auto password = m_pending->password;
        const auto secretKey = m_pending->secretKey;
        AsyncTask::runThenCallback(
            [kdf, password, secretKey] { return deriveAuk(stretchPassword(password, kdf), secretKey); },
            this,
            [this, record](const SecureBytes& auk) {
                if (!m_pending) {
                    return;
                }
                auto keyset = generateKeyset();
                const auto wrapped = wrappedKeysetJson(auk, keyset);
                keyset.clear();
                const auto device = AuthClient::DeviceInfo::current();
                m_client->registerFinish(m_pending->sessionId,
                                         record,
                                         m_pending->kdf.toJson(),
                                         wrapped,
                                         device,
                                         [this](const AuthClient::Response& response) {
                                             if (!m_pending) {
                                                 return;
                                             }
                                             if (!response.isSuccess()) {
                                                 fail(response.status == 410
                                                          ? tr("The registration took too long. Try again.")
                                                          : AuthClient::errorMessage(response));
                                                 return;
                                             }
                                             finishLogin(response.json());
                                         });
            });
    });
}

void AccountManager::signIn(const QString& email, const QString& password, const QString& secretKey)
{
    if (!m_config.isValid()) {
        emit error(tr("Choose an account server first."));
        return;
    }
    m_pending = std::make_unique<Pending>();
    m_pending->email = email.trimmed();
    m_pending->password = normalizePassword(password);
    const auto secretKeyText =
        secretKey.trimmed().isEmpty() ? storedSecretKey(defaultServerId(), m_pending->email) : secretKey;
    if (secretKeyText.isEmpty()) {
        fail(tr("Enter your Secret Key: this device hasn't signed in to this account before."));
        return;
    }
    if (!parseSecretKey(secretKeyText, &m_pending->secretKey)) {
        fail(tr("The Secret Key isn't valid. It looks like K1-XXXXXX-XXXXX-XXXXX-XXXXX-XXXXX."));
        return;
    }
    setState(State::SigningIn);

    QByteArray ke1;
    if (!m_opaque->createCredentialRequest(m_pending->password, &m_pending->opaqueSecret, &ke1)) {
        fail(OpaqueClient::isAvailable() ? tr("Could not start signing in.")
                                         : tr("This build of OpenVireo can't sign in to accounts."));
        return;
    }
    m_client->loginStart(m_pending->email, ke1, [this](const AuthClient::Response& response) {
        if (!m_pending) {
            return;
        }
        if (!response.isSuccess()) {
            fail(AuthClient::errorMessage(response));
            return;
        }
        const auto json = response.json();
        m_pending->sessionId = json.value("session_id").toString();
        QByteArray authU;
        // A wrong password (or an unknown email, which gets a fake record) fails here, on this device
        if (!m_opaque->recoverCredentials(fromBase64Url(json.value("ke2").toString()),
                                          m_pending->opaqueSecret,
                                          opaqueIds(m_pending->email),
                                          &authU)) {
            fail(tr("The email address or the master password is wrong."));
            return;
        }
        const auto device =
            AuthClient::DeviceInfo::current(SyncTokenStore::load(deviceKey(defaultServerId(), m_pending->email)));
        m_client->loginFinish(m_pending->sessionId, authU, device, [this](const AuthClient::Response& response) {
            if (!m_pending) {
                return;
            }
            if (!response.isSuccess()) {
                fail(response.status == 401 ? tr("The email address or the master password is wrong.")
                                            : AuthClient::errorMessage(response));
                return;
            }
            const auto json = response.json();
            if (json.value("mfa_required").toBool()) {
                m_pending->mfaToken = json.value("mfa_token").toString();
                QStringList methods;
                for (const auto& method : json.value("methods").toArray()) {
                    methods << method.toString();
                }
                setState(State::NeedsMfa);
                emit mfaRequired(methods.isEmpty() ? QStringList{"totp"} : methods);
                return;
            }
            finishLogin(json);
        });
    });
}

void AccountManager::submitMfa(const QString& totp)
{
    if (m_state != State::NeedsMfa || !m_pending) {
        return;
    }
    setState(State::SigningIn);
    m_client->loginMfa(m_pending->mfaToken, totp, [this](const AuthClient::Response& response) {
        if (!m_pending) {
            return;
        }
        if (!response.isSuccess()) {
            if (response.errorCode() == "invalid_mfa") {
                // Let the user try another code; after the 5th wrong one the server drops the token
                setState(State::NeedsMfa);
                emit error(tr("The code is wrong or has expired."));
                return;
            }
            fail(AuthClient::errorMessage(response));
            return;
        }
        finishLogin(response.json());
    });
}

void AccountManager::finishLogin(const QJsonObject& session)
{
    const auto user = session.value("user").toObject();
    m_serviceAccount = false;
    m_accountId = user.value("id").toString();
    m_email = user.value("email").toString(m_pending->email);
    m_deviceId = session.value("device_id").toString();
    setTokens(session.value("access_token").toString(),
              session.value("refresh_token").toString(),
              session.value("expires_in").toInt());

    deriveKeyset(
        session.value("kdf").toObject(), session.value("keyset").toObject(), [this, session](bool ok, const QString& message) {
            if (!ok) {
                // The server accepted the password, so the Secret Key is the likely culprit.
                // Don't leave the session the server just opened behind.
                m_pending.reset();
                signOut();
                emit error(message);
                return;
            }
            const bool registering = m_pending->registering;
            const auto secretKey = formatSecretKey(m_pending->secretKey);
            // This device is trusted from now on: it keeps the Secret Key
            SyncTokenStore::save(secretKeyKey(defaultServerId(), m_email), secretKey);
            SyncTokenStore::save(deviceKey(defaultServerId(), m_email), m_deviceId);
            m_pending.reset();
            saveSession();
            rememberCurrentAccount();
            emit knownAccountsChanged();
            SyncServerClient::setAccountTokenProvider(m_accountId, tokenProvider());
            AccountOfflineCache::saveProfile(m_accountId, session.value("kdf").toObject(),
                                             session.value("keyset").toObject(), {});
            setOffline(false);
            setState(State::Unlocked);
            if (registering) {
                emit registered(secretKey);
            }
            emit signedIn();
            reloadVaults();
            startEvents();
        });
}

void AccountManager::deriveKeyset(const QJsonObject& kdfJson,
                                  const QJsonObject& wrappedKeyset,
                                  std::function<void(bool ok, const QString& error)> done)
{
    bool kdfOk = false;
    auto kdf = KdfParams::fromJson(kdfJson, &kdfOk);
    // Low settings from the server would make the AUK cheap to guess
    if (!kdfOk || !kdf.isAcceptable() || kdf.memoryKiB < m_config.kdfMinMemoryKiB
        || kdf.iterations < m_config.kdfMinIterations) {
        done(false, tr("The account server sent unsafe key derivation settings."));
        return;
    }
    const auto password = m_pending->password;
    const auto secretKey = m_pending->secretKey;
    AsyncTask::runThenCallback(
        [kdf, password, secretKey] { return deriveAuk(stretchPassword(password, kdf), secretKey); },
        this,
        [this, wrappedKeyset, done](const SecureBytes& auk) {
            Keyset keyset;
            if (!unwrapKeyset(auk, wrappedKeyset, &keyset)) {
                done(false, tr("The Secret Key or the master password is wrong."));
                return;
            }
            m_keyset = keyset;
            // From now on Quick Unlock can stand in for the master password
            rememberAuk(auk);
            done(true, {});
        });
}

bool AccountManager::restoreSession(const QString& accountId)
{
    const auto json = QJsonDocument::fromJson(SyncTokenStore::load(sessionKey(accountId)).toUtf8()).object();
    const auto refreshToken = json.value("refresh_token").toString();
    if (accountId.isEmpty() || refreshToken.isEmpty()) {
        return false;
    }
    setServer(ServerConfig::fromJson(json.value("server").toObject()));
    m_serviceAccount = false;
    m_accountId = accountId;
    m_email = json.value("email").toString();
    m_deviceId = json.value("device_id").toString();
    m_refreshToken = refreshToken;
    m_accessToken.clear();
    m_accessTokenExpiry = 0;
    SyncServerClient::setAccountTokenProvider(m_accountId, tokenProvider());
    rememberCurrentAccount();
    setState(State::Locked);
    emit knownAccountsChanged();
    return true;
}

void AccountManager::whenTokensSettled(std::function<void()> done)
{
    QPointer<AccountManager> guard(this);
    if (m_setAsideChecks > 0) {
        m_setAsideCheckWaiters << [guard, done] {
            if (guard) {
                guard->whenTokensSettled(done);
            }
        };
    } else if (m_refreshing) {
        // Runs after the rotated token is saved, or after the session ended
        m_tokenWaiters << [done](const QString&) { done(); };
    } else {
        done();
    }
}

void AccountManager::setAside(std::function<void()> done)
{
    QPointer<AccountManager> guard(this);
    whenTokensSettled([guard, done] {
        if (!guard) {
            return;
        }
        auto* self = guard.data();
        if (self->m_serviceAccount || self->m_accountId.isEmpty() || self->m_refreshToken.isEmpty()) {
            done();
            return;
        }
        self->lock();
        self->stopEvents();
        self->m_refreshTimer.stop();
        SyncServerClient::setAccountTokenProvider(self->m_accountId, nullptr);
        self->m_accessToken.clear();
        self->m_refreshToken.clear();
        self->m_accessTokenExpiry = 0;
        self->m_accountId.clear();
        self->m_email.clear();
        self->m_deviceId.clear();
        self->setState(State::SignedOut);
        emit self->vaultsChanged();
        emit self->knownAccountsChanged();
        done();
    });
}

void AccountManager::switchTo(const QString& accountId, std::function<void(bool ok)> done)
{
    QPointer<AccountManager> guard(this);
    setAside([guard, accountId, done] {
        if (!guard || !guard->restoreSession(accountId)) {
            done(false);
            return;
        }
        SyncTokenStore::save(LastAccountKey, accountId);
        done(true);
    });
}

void AccountManager::signOutKnown(const QString& accountId)
{
    for (const auto& account : knownAccounts()) {
        if (account.id == accountId && !account.current) {
            // As signing out of a locked account: without a live token the server isn't told
            SyncTokenStore::remove(sessionKey(accountId));
            AccountOfflineCache::clear(accountId);
            auto* provider = m_quickUnlock ? m_quickUnlock : getQuickUnlock();
            provider->reset(quickUnlockIdOf(account.server.serverId(), accountId));
        }
    }
    emit knownAccountsChanged();
}

void AccountManager::forgetKnown(const QString& accountId)
{
    auto list = knownAccounts();
    for (const auto& account : list) {
        if (account.id == accountId && !account.current) {
            signOutKnown(accountId);
            SyncTokenStore::remove(secretKeyKey(account.server.serverId(), account.email));
            SyncTokenStore::remove(deviceKey(account.server.serverId(), account.email));
            list.removeIf([&](const KnownAccount& other) { return other.id == accountId; });
            saveKnownAccounts(list);
            break;
        }
    }
    emit knownAccountsChanged();
}

void AccountManager::checkSetAsideAccounts()
{
    for (const auto& account : knownAccounts()) {
        if (account.current || !account.signedIn || !account.server.isValid()
            || (account.checkedAt > 0 && now() - account.checkedAt < SetAsideCheckIntervalSec)) {
            continue;
        }
        const auto id = account.id;
        auto session = QJsonDocument::fromJson(SyncTokenStore::load(sessionKey(id)).toUtf8()).object();
        const auto token = session.value("refresh_token").toString();
        if (token.isEmpty()) {
            continue;
        }
        auto* client = account.server.api == m_config.api ? m_client : new AuthClient(account.server.api, this);
        ++m_setAsideChecks;
        client->refresh(token, [this, client, id, session](const AuthClient::Response& response) mutable {
            if (client != m_client) {
                client->deleteLater();
            }
            if (response.isSuccess()) {
                // Every refresh replaces the refresh token, here as for the open account
                session["refresh_token"] = response.json().value("refresh_token").toString();
                SyncTokenStore::save(sessionKey(id), QJsonDocument(session).toJson(QJsonDocument::Compact));
                auto list = knownAccounts();
                for (auto& known : list) {
                    if (known.id == id) {
                        known.checkedAt = now();
                    }
                }
                saveKnownAccounts(list);
            } else if (response.status == 400 || response.status == 401 || response.status == 403) {
                // The session is over on the server: that account's data goes, and no other's (SPEC §20)
                signOutKnown(id);
            }
            finishSetAsideCheck();
        });
    }
}

void AccountManager::finishSetAsideCheck()
{
    if (--m_setAsideChecks > 0) {
        return;
    }
    const auto waiters = std::move(m_setAsideCheckWaiters);
    m_setAsideCheckWaiters.clear();
    for (const auto& waiter : waiters) {
        waiter();
    }
    emit knownAccountsChanged();
}

void AccountManager::unlock(const QString& password)
{
    if (m_state != State::Locked || m_serviceAccount) {
        return;
    }
    const auto secretKeyText = storedSecretKey(defaultServerId(), m_email);
    m_pending = std::make_unique<Pending>();
    m_pending->email = m_email;
    m_pending->password = normalizePassword(password);
    if (!parseSecretKey(secretKeyText, &m_pending->secretKey)) {
        fail(tr("This device no longer has the account's Secret Key. Sign in again."));
        return;
    }
    setState(State::SigningIn);
    // The keyset and its KDF settings come from the server; reading them needs a valid access token. When
    // the server can't be reached, the copy this device kept at its last unlock stands in (SPEC §20).
    m_client->me([this](const AuthClient::Response& response) {
        if (!m_pending) {
            return;
        }
        QJsonObject profile;
        const bool offline = !response.isSuccess();
        if (response.isSuccess()) {
            profile = response.json();
            followEmail(profile.value("email").toString());
        } else if (response.isUnreachable() && hasOfflineCache()) {
            profile = AccountOfflineCache::load(m_accountId);
        } else {
            fail(unreachableMessage(response));
            return;
        }
        deriveKeyset(profile.value("kdf").toObject(),
                     profile.value("keyset").toObject(),
                     [this, profile, offline](bool ok, const QString& message) {
                         m_pending.reset();
                         if (!ok) {
                             setState(State::Locked);
                             emit error(message);
                             return;
                         }
                         if (!offline) {
                             takeProfile(profile);
                         }
                         finishUnlock(offline);
                     });
    });
}

void AccountManager::quickUnlock()
{
    if (!canQuickUnlock()) {
        return;
    }
    auto* provider = m_quickUnlock ? m_quickUnlock : getQuickUnlock();
    // Asks for Touch ID, Windows Hello or Polkit, as a database's Quick Unlock does
    QByteArray key;
    if (!provider->getKey(quickUnlockId(), key)) {
        emit error(tr("Failed to authenticate with Quick Unlock: %1").arg(provider->errorString()));
        return;
    }
    if (key.isEmpty()) {
        // The user cancelled
        return;
    }
    const auto auk = toSecure(key);
    Botan::secure_scrub_memory(key.data(), key.size());
    m_pending = std::make_unique<Pending>();
    m_pending->email = m_email;
    setState(State::SigningIn);
    m_client->me([this, auk](const AuthClient::Response& response) {
        if (!m_pending) {
            return;
        }
        QJsonObject profile;
        const bool offline = !response.isSuccess();
        if (response.isSuccess()) {
            profile = response.json();
            followEmail(profile.value("email").toString());
        } else if (response.isUnreachable() && hasOfflineCache()) {
            profile = AccountOfflineCache::load(m_accountId);
        } else {
            fail(unreachableMessage(response));
            return;
        }
        m_pending.reset();
        Keyset keyset;
        if (!unwrapKeyset(auk, profile.value("keyset").toObject(), &keyset)) {
            // The keyset was wrapped again, e.g. the master password changed on another device
            forgetAuk();
            setState(State::Locked);
            emit error(tr("Quick Unlock no longer opens this account, e.g. because its master password changed. "
                          "Unlock it with the master password."));
            return;
        }
        m_keyset = keyset;
        if (!offline) {
            takeProfile(profile);
        }
        finishUnlock(offline);
    });
}

void AccountManager::finishUnlock(bool offline)
{
    if (!offline) {
        setOffline(false);
        setState(State::Unlocked);
        reloadVaults();
        startEvents();
        return;
    }
    // The vault keys as GET /vaults last listed them; nothing goes out until the servers answer again
    const auto cache = AccountOfflineCache::load(m_accountId);
    m_orgs = parseOrgs(cache.value("orgs").toArray());
    m_keyring.setVaults(cache.value("vaults").toArray(), m_keyset.encSk);
    setOffline(true);
    setState(State::Unlocked);
    emit orgsChanged();
    emit vaultsChanged();
}

void AccountManager::setOffline(bool offline)
{
    if (offline) {
        scheduleReconnect();
    } else {
        m_reconnectTimer.stop();
        m_reconnectDelaySec = 0;
    }
    if (m_offline != offline) {
        m_offline = offline;
        emit offlineChanged(offline);
    }
}

void AccountManager::scheduleReconnect()
{
    m_reconnectDelaySec = m_reconnectDelaySec == 0 ? FirstReconnectDelaySec
                                                   : qMin(m_reconnectDelaySec * 2, MaxReconnectDelaySec);
    m_reconnectTimer.start(m_reconnectDelaySec * 1000);
}

void AccountManager::reconnect()
{
    if (m_state != State::Unlocked || !m_offline) {
        return;
    }
    m_reconnectTimer.stop();
    // Back online once GET /vaults answers; reloadOrgs() refreshes the cached keyset on the way
    reloadVaults();
}

void AccountManager::takeProfile(const QJsonObject& me)
{
    m_orgs = parseOrgs(me.value("orgs").toArray());
    setPendingDeletion(me.value("deletion_scheduled_for").toString());
    AccountOfflineCache::saveProfile(
        m_accountId, me.value("kdf").toObject(), me.value("keyset").toObject(), me.value("orgs").toArray());
}

QString AccountManager::unreachableMessage(const AuthClient::Response& response) const
{
    if (!response.isUnreachable()) {
        return AuthClient::errorMessage(response);
    }
    const auto host = hostOf(m_config.issuer.isEmpty() ? m_config.api : m_config.issuer);
    if (AccountOfflineCache::isEnabled()) {
        return tr("Can't reach %1, and this device hasn't kept a copy to unlock offline yet. Check your "
                  "connection and try again.")
            .arg(host);
    }
    return tr("Can't reach %1. Check your connection and try again.").arg(host);
}

void AccountManager::lock()
{
    setOffline(false);
    m_pending.reset();
    m_keyring.clear();
    m_keyset.clear();
    m_orgs.clear();
    if (m_state == State::Unlocked || m_state == State::SigningIn || m_state == State::NeedsMfa) {
        if (m_serviceAccount) {
            // A service account has no session to keep without its token
            endSession({});
            return;
        }
        setState(m_refreshToken.isEmpty() ? State::SignedOut : State::Locked);
        emit vaultsChanged();
    }
}

void AccountManager::signOut()
{
    if (!m_serviceAccount && !m_accessToken.isEmpty() && m_accessTokenExpiry > now()) {
        // Best effort: the server revokes this device's refresh tokens. The request takes the current
        // token with it before endSession() forgets it; nobody waits for the answer.
        m_client->logout([](const AuthClient::Response&) {});
    }
    endSession({});
}

void AccountManager::endSession(const QString& reason)
{
    stopEvents();
    m_refreshTimer.stop();
    m_pending.reset();
    // Like closing a database: its Quick Unlock goes with the session
    forgetAuk();
    m_orgs.clear();
    setOffline(false);
    if (!m_accountId.isEmpty()) {
        SyncServerClient::setAccountTokenProvider(m_accountId, nullptr);
        if (!m_serviceAccount) {
            SyncTokenStore::remove(sessionKey(m_accountId));
            // The offline cache goes with the session (SPEC §20)
            AccountOfflineCache::clear(m_accountId);
        }
    }
    m_accessToken.clear();
    m_refreshToken.clear();
    m_accessTokenExpiry = 0;
    m_saToken = {};
    m_keyring.clear();
    m_keyset.clear();
    m_accountId.clear();
    m_serviceAccount = false;
    m_deletionScheduledFor.clear();
    const auto waiters = std::move(m_tokenWaiters);
    m_tokenWaiters.clear();
    m_refreshing = false;
    for (const auto& waiter : waiters) {
        waiter({});
    }
    const bool wasSignedIn = m_state == State::Locked || m_state == State::Unlocked;
    setState(State::SignedOut);
    if (wasSignedIn) {
        emit vaultsChanged();
        emit signedOut(reason);
    }
    emit knownAccountsChanged();
}

void AccountManager::followEmail(const QString& email)
{
    // The Secret Key and device id are kept under the email: move them when it changed elsewhere, so
    // signing in again here doesn't ask for the Secret Key (SPEC §16)
    if (email.isEmpty() || m_serviceAccount || OpaqueClient::userId(email) == OpaqueClient::userId(m_email)) {
        return;
    }
    const auto server = defaultServerId();
    for (const auto& key : {&AccountManager::secretKeyKey, &AccountManager::deviceKey}) {
        const auto value = SyncTokenStore::load(key(server, m_email));
        if (!value.isEmpty()) {
            SyncTokenStore::save(key(server, email), value);
            SyncTokenStore::remove(key(server, m_email));
        }
    }
    m_email = email;
    saveSession();
    rememberCurrentAccount();
    emit emailChanged();
    emit knownAccountsChanged();
}

void AccountManager::saveSession() const
{
    if (m_serviceAccount || m_accountId.isEmpty()) {
        return;
    }
    QJsonObject json{{"refresh_token", m_refreshToken},
                     {"email", m_email},
                     {"device_id", m_deviceId},
                     {"server", m_config.toJson()}};
    SyncTokenStore::save(sessionKey(m_accountId), QJsonDocument(json).toJson(QJsonDocument::Compact));
    SyncTokenStore::save(LastAccountKey, m_accountId);
}

void AccountManager::setTokens(const QString& accessToken, const QString& refreshToken, int expiresIn)
{
    m_accessToken = accessToken;
    if (!refreshToken.isEmpty()) {
        m_refreshToken = refreshToken;
    }
    if (expiresIn <= 0) {
        // Fall back to the token's own expiry
        JwtClaims claims;
        expiresIn = parseJwtClaims(accessToken, &claims) && claims.exp > 0 ? static_cast<int>(claims.exp - now()) : 0;
    }
    m_accessTokenExpiry = now() + expiresIn;
    m_refreshTimer.start(qMax(1, expiresIn - RefreshMarginSec) * 1000);
    emit accessTokenChanged();
}

void AccountManager::accessToken(std::function<void(const QString&)> done)
{
    if (QThread::currentThread() != thread()) {
        QPointer<AccountManager> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, done] {
                if (guard) {
                    guard->accessToken(done);
                } else {
                    done({});
                }
            },
            Qt::QueuedConnection);
        return;
    }
    if (!m_accessToken.isEmpty() && m_accessTokenExpiry - now() > MinTokenLifetimeSec) {
        done(m_accessToken);
        return;
    }
    if (m_refreshToken.isEmpty() && !(m_serviceAccount && !m_saToken.secret.empty())) {
        done({});
        return;
    }
    m_tokenWaiters << done;
    refreshAccessToken();
}

AccessTokenProvider AccountManager::tokenProvider()
{
    QPointer<AccountManager> guard(this);
    return [guard](std::function<void(const QString&)> done) {
        if (auto* manager = guard.data()) {
            manager->accessToken(std::move(done));
        } else {
            done({});
        }
    };
}

void AccountManager::refreshAccessToken()
{
    if (m_refreshing) {
        return;
    }
    if (m_serviceAccount) {
        exchangeServiceAccount();
        return;
    }
    if (m_refreshToken.isEmpty()) {
        finishRefresh(false);
        return;
    }
    m_refreshing = true;
    m_client->refresh(m_refreshToken, [this](const AuthClient::Response& response) {
        m_refreshing = false;
        if (response.isSuccess()) {
            const auto json = response.json();
            setTokens(json.value("access_token").toString(),
                      json.value("refresh_token").toString(),
                      json.value("expires_in").toInt());
            // Every refresh replaces the refresh token; the old one is now a reuse alarm on the server
            saveSession();
            finishRefresh(true);
            return;
        }
        if (response.status == 400 || response.status == 401 || response.status == 403) {
            // Revoked, expired or reused (token_reused): the session is over on the server
            endSession(response.errorCode() == "token_reused"
                           ? tr("Your session was ended because its sign-in token was used twice. Sign in again.")
                           : tr("Your session has ended. Sign in again."));
            return;
        }
        // The network or the server is down: keep the session and try again later
        m_refreshTimer.start(30 * 1000);
        finishRefresh(false);
    });
}

void AccountManager::finishRefresh(bool ok)
{
    const auto waiters = std::move(m_tokenWaiters);
    m_tokenWaiters.clear();
    // A token that hasn't expired yet is still better than none
    const auto token = ok || m_accessTokenExpiry > now() ? m_accessToken : QString();
    for (const auto& waiter : waiters) {
        waiter(token);
    }
    if (ok && m_reloadAfterRefresh) {
        m_reloadAfterRefresh = false;
        reloadVaults();
        // Reconnect so the stream covers the vaults of the new token
        stopEvents();
        startEvents();
    }
}

void AccountManager::signInServiceAccount(const QString& token)
{
    ServiceAccountToken parsed;
    if (!parseServiceAccountToken(token, &parsed)) {
        emit error(tr("This isn't a valid service account token."));
        return;
    }
    endSession({});
    m_serviceAccount = true;
    m_saToken = parsed;
    m_accountId = parsed.saId;
    setState(State::SigningIn);
    if (m_config.issuer.isEmpty() || QUrl(m_config.issuer) != QUrl(parsed.url)) {
        // The token names its server: ask it where its API is
        m_client->discover(parsed.url, [this](const AuthClient::Response& response) {
            const auto config = ServerConfig::fromJson(response.json());
            if (!response.isSuccess() || !config.isValid()) {
                endSession({});
                emit error(AuthClient::errorMessage(response));
                return;
            }
            if (const auto insecure = config.insecureUrl(); !insecure.isEmpty()) {
                endSession({});
                emit error(insecureServerMessage(insecure));
                return;
            }
            setServer(config);
            exchangeServiceAccount();
        });
        return;
    }
    exchangeServiceAccount();
}

void AccountManager::exchangeServiceAccount()
{
    if (m_refreshing || m_saToken.secret.empty()) {
        return;
    }
    m_refreshing = true;
    m_client->exchangeServiceAccount(
        m_saToken.saId, toBase64Url(m_saToken.secret), [this](const AuthClient::Response& response) {
            m_refreshing = false;
            if (!response.isSuccess()) {
                const bool first = m_state == State::SigningIn;
                const auto message = AuthClient::errorMessage(response);
                if (first || response.status == 401) {
                    endSession(tr("The service account token was rejected."));
                    emit error(message);
                } else {
                    m_refreshTimer.start(30 * 1000);
                    finishRefresh(false);
                }
                return;
            }
            const auto json = response.json();
            setTokens(json.value("access_token").toString(), {}, json.value("expires_in").toInt());
            if (m_keyset.isNull()) {
                // kek -> SA master key -> SA keyset; the server holds neither the kek nor the master key
                SecureBytes master;
                Keyset keyset;
                if (!unwrap(m_saToken.kek,
                            fromBase64Url(json.value("wrapped_master").toString()),
                            saMasterAad(m_saToken.saId),
                            &master)
                    || !unwrapKeyset(master, json.value("keyset").toObject(), &keyset)) {
                    endSession({});
                    emit error(tr("The service account's keys could not be decrypted."));
                    return;
                }
                m_keyset = keyset;
                SyncServerClient::setAccountTokenProvider(m_accountId, tokenProvider());
                setState(State::Unlocked);
                emit signedIn();
                reloadVaults();
                startEvents();
            }
            finishRefresh(true);
        });
}

void AccountManager::reloadVaults()
{
    if (m_state != State::Unlocked) {
        return;
    }
    m_client->listVaults([this](const AuthClient::Response& response) {
        if (m_state != State::Unlocked) {
            return;
        }
        if (!response.isSuccess()) {
            if (response.isUnreachable() && !m_serviceAccount) {
                // Work from what is open and cached until the servers answer again
                if (m_offline) {
                    scheduleReconnect();
                } else {
                    setOffline(true);
                }
                return;
            }
            if (response.errorCode() == "account_pending_deletion") {
                // SPEC §23: nothing opens until the account is restored
                setPendingDeletion(response.json().value("scheduled_for").toString());
                return;
            }
            emit error(AuthClient::errorMessage(response));
            return;
        }
        const auto vaults = response.json().value("vaults").toArray();
        if (!m_serviceAccount) {
            AccountOfflineCache::saveVaults(m_accountId, vaults);
        }
        m_keyring.setVaults(vaults, m_keyset.encSk);
        if (m_offline) {
            setOffline(false);
            startEvents();
        }
        emit vaultsChanged();
    });
    reloadOrgs();
}

void AccountManager::reloadOrgs()
{
    // Service accounts belong to one org and have no /me
    if (m_serviceAccount) {
        return;
    }
    m_client->me([this](const AuthClient::Response& response) {
        if (m_state != State::Unlocked || !response.isSuccess()) {
            return;
        }
        const auto json = response.json();
        followEmail(json.value("email").toString());
        const auto encPk = fromBase64Url(json.value("keyset").toObject().value("enc_pk").toString());
        if (!encPk.isEmpty() && encPk != m_keyset.publicKeys.encPk) {
            // A new keyset (account recovery): the vault keys in memory are sealed to the old one
            lock();
            emit error(tr("Your account's keys changed on another device. Unlock it again."));
            return;
        }
        takeProfile(json);
        emit orgsChanged();
    });
}

void AccountManager::handleAccessEvent(const SyncEventStream::Event& event)
{
    if (m_state == State::SignedOut) {
        return;
    }
    // SPEC §23: deleted here or on another device
    if (event.change == "deletion_scheduled") {
        endSession(tr("This account is scheduled for deletion. Sign in to restore it."));
        return;
    }
    if (event.change == "account_deleted") {
        const auto server = defaultServerId();
        const auto email = m_email;
        auto list = knownAccounts();
        list.removeIf([this](const KnownAccount& a) { return a.id == m_accountId; });
        saveKnownAccounts(list);
        endSession(tr("This account was deleted."));
        SyncTokenStore::remove(secretKeyKey(server, email));
        SyncTokenStore::remove(deviceKey(server, email));
        emit knownAccountsChanged();
        return;
    }
    // The new access shows in the token's vlt claim and in GET /vaults
    m_reloadAfterRefresh = true;
    refreshAccessToken();
}

RemoteParams AccountManager::vaultRemoteParams(const QString& vaultId) const
{
    RemoteParams params;
    const auto name = m_keyring.vault(vaultId).name;
    params.name = name.isEmpty() ? tr("OpenVireo account") : tr("OpenVireo account (%1)").arg(name);
    params.type = RemoteParams::Type::AccountVault;
    params.serverUrl = m_config.sync;
    params.accountId = m_accountId;
    params.vaultId = vaultId;
    params.autoSync = true;
    return params;
}

void AccountManager::createVault(const QString& name, CreateVaultCallback done)
{
    if (m_state != State::Unlocked || m_serviceAccount) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    const auto vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto vaultKey = generateVaultKey();
    const auto sealed = seal(m_keyset.publicKeys.encPk, vaultKey, vaultKeyAad(vaultId));
    const auto encryptedName = encryptVaultName(vaultKey, vaultId, name);
    const QJsonArray grants{AuthClient::grant(principal(), "manage", 1, sealed)};
    m_client->createVault(vaultId,
                          {},
                          encryptedName,
                          grants,
                          [this, vaultId, vaultKey, name, done](const AuthClient::Response& response) {
                              if (!response.isSuccess()) {
                                  done({}, {}, AuthClient::errorMessage(response));
                                  return;
                              }
                              // The new vault is in the next token's vlt claim, which the sync server needs to accept
                              // the upload
                              m_reloadAfterRefresh = true;
                              refreshAccessToken();
                              done(vaultId, VaultKeyring::createVaultDatabase(name, vaultKey), {});
                          });
}

void AccountManager::startEvents()
{
    if (!m_eventsEnabled || m_events || m_config.sync.isEmpty() || m_state != State::Unlocked) {
        return;
    }
    RemoteParams params;
    params.type = RemoteParams::Type::AccountVault;
    params.serverUrl = m_config.sync;
    params.accountId = m_accountId;
    params.tokenProvider = tokenProvider();
    m_events = new SyncEventStream(params, this);
    connect(m_events, &SyncEventStream::accessChanged, this, &AccountManager::handleAccessEvent);
    connect(m_events, &SyncEventStream::databaseChanged, this, &AccountManager::vaultChanged);
    m_events->start();
}

void AccountManager::stopEvents()
{
    delete m_events.data();
}

// ------------------------------------------------------------------ account deletion (SPEC §23)

QString AccountManager::deletionScheduledFor() const
{
    return m_deletionScheduledFor;
}

void AccountManager::setPendingDeletion(const QString& scheduledFor)
{
    if (m_serviceAccount || scheduledFor == m_deletionScheduledFor) {
        return;
    }
    m_deletionScheduledFor = scheduledFor;
    setKnownDeletion(m_accountId, scheduledFor);
    emit knownAccountsChanged();
    emit pendingDeletionChanged();
}

void AccountManager::setKnownDeletion(const QString& accountId, const QString& scheduledFor) const
{
    auto list = knownAccounts();
    for (auto& account : list) {
        if (account.id == accountId) {
            account.deletionScheduledFor = scheduledFor;
        }
    }
    saveKnownAccounts(list);
}

void AccountManager::removeErasedAccounts()
{
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
    bool changed = false;
    for (const auto& account : knownAccounts()) {
        const auto at = QDateTime::fromString(account.deletionScheduledFor, Qt::ISODate);
        if (!at.isValid() || at > cutoff || account.current) {
            continue;
        }
        const auto server = account.server.serverId();
        SyncTokenStore::remove(sessionKey(account.id));
        SyncTokenStore::remove(secretKeyKey(server, account.email));
        SyncTokenStore::remove(deviceKey(server, account.email));
        AccountOfflineCache::clear(account.id);
        auto list = knownAccounts();
        list.removeIf([&](const KnownAccount& a) { return a.id == account.id; });
        saveKnownAccounts(list);
        changed = true;
    }
    if (changed) {
        emit knownAccountsChanged();
    }
}

void AccountManager::freshToken(std::function<void(bool ok)> done)
{
    // DELETE /me and POST /me/deletion/cancel want a token issued in the last 5 minutes
    m_tokenWaiters << [done](const QString& token) { done(!token.isEmpty()); };
    refreshAccessToken();
}

void AccountManager::verifyPassword(const QString& password, std::function<void(const QString& error)> done)
{
    SecureBytes secretKey;
    if (!parseSecretKey(storedSecretKey(defaultServerId(), m_email), &secretKey)) {
        done(tr("This device no longer has the account's Secret Key. Sign in again."));
        return;
    }
    const auto normalized = normalizePassword(password);
    QPointer<AccountManager> guard(this);
    m_client->me([this, guard, secretKey, normalized, done](const AuthClient::Response& response) {
        if (!guard) {
            return;
        }
        if (!response.isSuccess()) {
            done(AuthClient::errorMessage(response));
            return;
        }
        const auto me = response.json();
        bool kdfOk = false;
        const auto kdf = KdfParams::fromJson(me.value("kdf").toObject(), &kdfOk);
        if (!kdfOk || !kdf.isAcceptable()) {
            done(tr("The account server sent unsafe key derivation settings."));
            return;
        }
        const auto wrapped = me.value("keyset").toObject();
        AsyncTask::runThenCallback(
            [kdf, normalized, secretKey] { return deriveAuk(stretchPassword(normalized, kdf), secretKey); },
            this,
            [wrapped, done](const SecureBytes& auk) {
                Keyset keyset;
                done(unwrapKeyset(auk, wrapped, &keyset) ? QString() : tr("That master password is wrong."));
            });
    });
}

void AccountManager::deletionPreview(std::function<void(const QJsonObject& preview, const QString& error)> done)
{
    m_client->deletionPreview([done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        done(response.json(), {});
    });
}

void AccountManager::deleteAccount(const QString& password,
                                   const QString& confirm,
                                   std::function<void(const QString& scheduledFor, const QString& error)> done)
{
    if (m_serviceAccount || m_state != State::Unlocked) {
        done({}, tr("Unlock your account first."));
        return;
    }
    QPointer<AccountManager> guard(this);
    verifyPassword(password, [this, guard, confirm, done](const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            done({}, error);
            return;
        }
        freshToken([this, guard, confirm, done](bool ok) {
            if (!guard) {
                return;
            }
            if (!ok) {
                done({}, tr("Your session has ended. Sign in again."));
                return;
            }
            m_client->scheduleDeletion(confirm.trimmed(), [this, guard, done](const AuthClient::Response& response) {
                if (!guard) {
                    return;
                }
                if (!response.isSuccess()) {
                    done({}, AuthClient::errorMessage(response));
                    return;
                }
                const auto when = response.json().value("scheduled_for").toString();
                // The server already ended every session; the Secret Key stays for a restore
                setKnownDeletion(m_accountId, when);
                const auto date = QLocale().toString(QDateTime::fromString(when, Qt::ISODate).toLocalTime().date(),
                                                     QLocale::LongFormat);
                endSession(tr("Your account is locked and will be deleted on %1. Sign in before then to restore it.")
                               .arg(date));
                done(when, {});
            });
        });
    });
}

void AccountManager::restoreAccount(const QString& password, std::function<void(const QString& error)> done)
{
    if (m_serviceAccount || m_deletionScheduledFor.isEmpty()) {
        done(tr("This account isn't scheduled for deletion."));
        return;
    }
    QPointer<AccountManager> guard(this);
    verifyPassword(password, [this, guard, done](const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            done(error);
            return;
        }
        freshToken([this, guard, done](bool ok) {
            if (!guard) {
                return;
            }
            if (!ok) {
                done(tr("Your session has ended. Sign in again."));
                return;
            }
            m_client->cancelDeletion([this, guard, done](const AuthClient::Response& response) {
                if (!guard) {
                    return;
                }
                if (!response.isSuccess()) {
                    done(AuthClient::errorMessage(response));
                    return;
                }
                // A token without `del`, which opens vaults again
                freshToken([this, guard, done](bool) {
                    if (!guard) {
                        return;
                    }
                    setPendingDeletion({});
                    reloadVaults();
                    startEvents();
                    done({});
                });
            });
        });
    });
}

void AccountManager::transferVault(const QString& vaultId,
                                   const QString& principal,
                                   std::function<void(const QString& error)> done)
{
    QPointer<AccountManager> guard(this);
    m_client->transferVault(vaultId, principal, [this, guard, done](const AuthClient::Response& response) {
        if (!guard) {
            return;
        }
        if (!response.isSuccess()) {
            done(AuthClient::errorMessage(response));
            return;
        }
        m_reloadAfterRefresh = true;
        refreshAccessToken();
        done({});
    });
}
