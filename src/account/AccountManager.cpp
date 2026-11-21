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
#include "core/Config.h"
#include "core/Database.h"
#include "core/Tools.h"
#include "format/KeePass2Reader.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/remote/SyncTokenStore.h"
#include "keys/CompositeKey.h"
#include "keys/FileKey.h"
#include "quickunlock/QuickUnlockInterface.h"

#include <botan/mem_ops.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

#include <QBuffer>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkInformation>
#endif
#include <QCoreApplication>
#include <QScopeGuard>
#include <QSysInfo>
#include <QThread>
#include <QtConcurrent>
#include <QUrl>
#include <QUuid>

using namespace AccountCrypto;

// The core client and its account for one server
struct AccountManager::CoreSession
{
    rust::Box<ov::Client> client;
    std::optional<rust::Box<ov::Account>> account;

    const ov::Account& acct() const
    {
        if (!account) {
            throw std::runtime_error("signed_out: no account server");
        }
        return **account;
    }
};

// What a sign-in or registration in progress keeps until the session arrives
struct AccountManager::Pending
{
    bool registering = false;
    QString email;
    std::shared_ptr<rust::Box<ov::TotpChallenge>> challenge;
};

struct AccountManager::VaultList
{
    QList<VaultKeyring::Vault> vaults;
    // vault id -> its newest key version and key, from Account::vault_kdbx_credentials
    QHash<QString, QPair<int, SecureBytes>> keys;
    // The core couldn't reach the server and listed what it cached
    bool offline = false;
};

// The core's event stream, read on a thread of its own: EventStream::next blocks until something happens
struct AccountManager::EventLoop
{
    std::thread thread;
    std::atomic_bool stop{false};
    std::atomic_bool done{false};
    std::mutex mutex;
    std::optional<rust::Box<ov::CancelToken>> cancel;
    std::optional<rust::Box<ov::EventStream>> stream;
};

struct AccountManager::SessionInfo
{
    QString userId;
    QString email;
    QString deviceId;
    QString accessToken;
    QString secretKey;
    QString deletion;
    bool offline = false;
    bool mfa = false;
    std::shared_ptr<rust::Box<ov::TotpChallenge>> challenge;
    SecureBytes keysetSecret;
    SecureBytes auk;
    // An offline unlock: the vault list the core cached
    VaultList vaults;
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
                                    json.value("status").toString(),
                                    QStringLiteral("on"),
                                    {}};
            org.shareLinks = json.value("item_share_links").toString("on");
            org.plan = AccountPlan::parse(json.value("plan"));
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


namespace
{
    rust::Vec<uint8_t> bytesOf(const QByteArray& data)
    {
        rust::Vec<uint8_t> vec;
        vec.reserve(static_cast<size_t>(data.size()));
        for (auto c : data) {
            vec.push_back(static_cast<uint8_t>(c));
        }
        return vec;
    }

    rust::Vec<uint8_t> secretOf(const QString& text)
    {
        auto utf8 = text.toUtf8();
        auto vec = bytesOf(utf8);
        Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
        return vec;
    }

    SecureBytes takeSecure(rust::Vec<uint8_t>& bytes)
    {
        SecureBytes out(bytes.begin(), bytes.end());
        ov::wipe(bytes);
        return out;
    }

    QString takeText(rust::Vec<uint8_t>& bytes)
    {
        auto text = QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
        ov::wipe(bytes);
        return text;
    }

    rust::String rs(const QString& text)
    {
        const auto utf8 = text.toUtf8();
        return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
    }

    QString qs(const rust::String& text)
    {
        return CoreHost::toQString(text);
    }

    // The reason in a sign-in error's details (contract-v1.7): keyset_unwrap_failed or opaque_refused
    QString signInReason(const AccountManager::CoreError& e)
    {
        return QJsonDocument::fromJson(e.details.toUtf8()).object().value("reason").toString();
    }

    // The keyset didn't unwrap on this device: the password or the Secret Key is wrong
    bool wrongKey(const AccountManager::CoreError& e)
    {
        return e.code == "decryption_failed" || e.code == "invalid_secret_key"
               || (e.code == "unauthorized" && signInReason(e) == "keyset_unwrap_failed");
    }

    // At sign-in the server accepted the password (OPAQUE) and only then did the keyset not unwrap: the Secret Key is
    // wrong. The core logged the new session out again.
    bool wrongSecretKey(const AccountManager::CoreError& e)
    {
        return e.code == "invalid_secret_key"
               || (e.code == "unauthorized" && signInReason(e) == "keyset_unwrap_failed");
    }

    ov::ServerConfig coreConfig(const AccountManager::ServerConfig& config)
    {
        ov::ServerConfig out;
        out.issuer = rs(config.issuer.isEmpty() ? config.api : config.issuer);
        out.api = rs(config.api);
        out.sync = rs(config.sync);
        out.opaque_ids = rs(config.serverId());
        out.has_kdf_min = true;
        out.kdf_min_m = config.kdfMinMemoryKiB;
        out.kdf_min_t = config.kdfMinIterations;
        out.web = rs(config.web);
        out.share = rs(config.share);
        return out;
    }

    // The token's lifetime from its own claims, so it doesn't matter whose clock issued it
    qint64 lifetimeOf(const QString& token)
    {
        JwtClaims claims;
        if (parseJwtClaims(token, &claims) && claims.exp > 0 && claims.iat > 0 && claims.exp > claims.iat) {
            return claims.exp - claims.iat;
        }
        return 600;
    }
} // namespace

namespace
{
    void readSession(const ov::Account& account, AccountManager::SessionInfo& info, bool withToken)
    {
        info.userId = qs(account.user_id());
        info.email = qs(account.email());
        const auto stored = account.stored_session();
        if (stored.has) {
            info.deviceId = qs(stored.value.device_id);
        }
        auto secret = account.unlocked_keyset_secret();
        info.keysetSecret = takeSecure(secret);
        if (withToken) {
            auto token = account.access_token(false, *ov::new_cancel_token());
            info.accessToken = takeText(token);
        }
    }

    QString roleName(ov::VaultRole role)
    {
        switch (role) {
        case ov::VaultRole::Manage:
            return QStringLiteral("manage");
        case ov::VaultRole::Write:
            return QStringLiteral("write");
        default:
            return QStringLiteral("read");
        }
    }

    QByteArray toBytes(const rust::Vec<uint8_t>& data)
    {
        return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    }

    // The vault list, names decrypted by the core, with each vault's newest key (cheap: the core holds the keyring
    // after listing). `cached`: the copy from the last listing, without the network.
    void loadVaultList(const ov::Account& account, AccountManager::VaultList& out, bool cached)
    {
        auto cancel = ov::new_cancel_token();
        rust::Vec<ov::VaultInfo> list;
        if (cached) {
            auto stored = account.cached_vaults();
            if (!stored.has) {
                return;
            }
            list = std::move(stored.vaults);
        } else {
            list = account.vaults(*cancel);
        }
        out.offline = account.is_offline();
        for (const auto& info : list) {
            VaultKeyring::Vault vault;
            vault.id = qs(info.id);
            vault.name = qs(info.name);
            vault.role = roleName(info.role);
            vault.orgId = qs(info.org_id);
            vault.currentKeyVersion = static_cast<int>(info.current_key_version);
            vault.pendingKeyVersion = info.has_pending_key_version ? static_cast<int>(info.pending_key_version) : -1;
            vault.rotationRequired = info.rotation_required;
            out.vaults << vault;
            try {
                auto credentials = account.vault_kdbx_credentials(info.id, *cancel);
                if (credentials.has_key_file) {
                    const int version = qMax(vault.currentKeyVersion, vault.pendingKeyVersion);
                    out.keys.insert(vault.id, qMakePair(version, takeSecure(credentials.key_file)));
                }
                ov::wipe(credentials.password);
            } catch (const std::exception& e) {
                // Listed, but no key of it is sealed to this account: it shows and doesn't open
                qWarning("No key for vault %s: %s", qPrintable(vault.id), e.what());
            }
        }
    }

    Keyset keysetOf(const SecureBytes& secret)
    {
        if (secret.size() != 64) {
            return {};
        }
        const SecureBytes encSk(secret.begin(), secret.begin() + 32);
        const SecureBytes signSeed(secret.begin() + 32, secret.end());
        return keysetFromSecrets(encSk, signSeed);
    }
} // namespace

AccountManager::AccountManager(QObject* parent)
    : AccountManager(CoreHost::Factory(), parent)
{
}

AccountManager::AccountManager(CoreHost::Factory host, QObject* parent)
    : QObject(parent)
    , m_host(std::move(host))
    , m_client(new AuthClient())
{
    // One core call at a time, in the order they were made: a sign-out can't overtake the sign-in after it
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(-1);
    m_client->setParent(this);
    m_client->setTransport(std::shared_ptr<ov::HttpTransport>(m_host.makeTransport()));
    m_client->setTokenProvider(tokenProvider());
    try {
        ov::ClientConfig config;
        config.device_name = rs(AuthClient::DeviceInfo::current().name);
        m_local = std::make_shared<CoreSession>(CoreSession{m_host.newClient(config), std::nullopt});
    } catch (const std::exception& e) {
        qWarning("openvireo-core client: %s", e.what());
    }
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
        unregisterAccountHooks();
    }
    m_keyring.clear();
    m_keyset.clear();
    // Calls under way finish (their results go nowhere): their Host may not outlive this manager. A test Host can
    // need this thread's event loop to answer, so it keeps turning meanwhile.
    m_destroying = true;
    m_client->abort();
    stopEvents();
    while (!m_pool.waitForDone(5)) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    delete m_client;
    m_client = nullptr;
}

std::shared_ptr<AccountManager::CoreSession> AccountManager::makeCore(const ServerConfig& config) const
{
    if (!config.isValid()) {
        return {};
    }
    try {
        ov::ClientConfig clientConfig;
        clientConfig.device_name = rs(AuthClient::DeviceInfo::current().name);
        // The core keeps its offline copies (profile, vault list, vaults) only when allowed (SPEC §20)
        clientConfig.offline = AccountOfflineCache::isEnabled();
        clientConfig.has_server = true;
        clientConfig.server = coreConfig(config);
        auto core = std::make_shared<CoreSession>(CoreSession{m_host.newClient(clientConfig), std::nullopt});
        // With the server given this reads the keychain only, no network
        auto cancel = ov::new_cancel_token();
        core->account = ov::call([&] { return core->client->connect(clientConfig.server.issuer, *cancel); });
        return core;
    } catch (const std::exception& e) {
        qWarning("openvireo-core account: %s", e.what());
        return {};
    }
}

void AccountManager::runCore(std::shared_ptr<CoreSession> core,
                             std::function<void(CoreSession&)> work,
                             std::function<void(const CoreError&)> done)
{
    auto error = std::make_shared<CoreError>();
    const auto fill = [](CoreError& out, const ov::ErrorInfo& info) {
        out.code = qs(info.code);
        out.message = qs(info.message);
        out.details = qs(info.details);
        out.serverCode = qs(info.server_code);
        out.status = info.status;
    };
    ++m_inFlight;
    auto future = QtConcurrent::run(&m_pool, [core, work, error, fill] {
        if (!core) {
            error->code = QStringLiteral("signed_out");
            error->message = QStringLiteral("no account server");
            return;
        }
        try {
            work(*core);
        } catch (const ov::Error& e) {
            fill(*error, e.info());
        } catch (const rust::Error&) {
            fill(*error, ov::take_last_error());
        } catch (const std::exception& e) {
            const QString what = QString::fromUtf8(e.what());
            const auto colon = what.indexOf(": ");
            error->code = colon > 0 ? what.left(colon) : QStringLiteral("internal");
            error->message = colon > 0 ? what.mid(colon + 2) : what;
        }
        if (error->code.isEmpty() && !error->message.isEmpty()) {
            error->code = QStringLiteral("internal");
        }
    });
    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, error, done] {
        watcher->deleteLater();
        --m_inFlight;
        if (done && !m_destroying) {
            done(*error);
        }
    });
    watcher->setFuture(future);
}

void AccountManager::callAccount(QObject* context, AccountWork work, AccountDone done)
{
    QPointer<QObject> guard(context);
    if (m_state != State::Unlocked || !m_core) {
        CoreError error;
        error.code = QStringLiteral("signed_out");
        error.message = tr("Unlock your account first.");
        QTimer::singleShot(0, this, [guard, done, error] {
            if (guard && done) {
                done(error);
            }
        });
        return;
    }
    auto core = m_core;
    runCore(
        core,
        [work](CoreSession& c) {
            auto cancel = ov::new_cancel_token();
            work(c.acct(), *cancel);
        },
        [this, guard, core, done](const CoreError& e) {
            if (!e.ok() && core == m_core) {
                sessionEndedBy(e);
            }
            if (guard && done) {
                done(e);
            }
        });
}

QString AccountManager::errorMessage(const CoreError& error) const
{
    return messageFor(error);
}

void AccountManager::setServer(const ServerConfig& config)
{
    const bool same = m_core && config.toJson() == m_config.toJson();
    m_config = config;
    m_client->setApiUrl(config.api);
    if (!same) {
        m_core = makeCore(config);
    }
    emit serverChanged();
}

AccountManager::ServerConfig AccountManager::server() const
{
    return m_config;
}

void AccountManager::discover(const QString& issuerUrl)
{
    if (!m_local) {
        emit error(tr("This build of OpenVireo can't sign in to accounts."));
        return;
    }
    auto result = std::make_shared<ServerConfig>();
    const auto issuer = issuerUrl;
    runCore(
        m_local,
        [result, issuer](CoreSession& core) {
            const auto c = core.client->discover(rs(issuer), *ov::new_cancel_token());
            result->issuer = qs(c.issuer);
            result->api = qs(c.api);
            result->sync = qs(c.sync);
            result->web = qs(c.web);
            result->share = qs(c.share);
            result->opaqueServerId = qs(c.opaque_ids);
            if (c.has_kdf_min) {
                result->kdfMinMemoryKiB = qMax<quint32>(KdfParams::MinMemoryKiB, c.kdf_min_m);
                result->kdfMinIterations = qMax<quint32>(KdfParams::MinIterations, c.kdf_min_t);
            }
        },
        [this, result](const CoreError& e) {
            if (!e.ok()) {
                // The core refuses insecure URLs too (SPEC §1)
                emit error(e.code == "insecure_url"
                               ? tr("The server sent an address that doesn't use https://. Plain http:// is only "
                                    "allowed for this computer.")
                               : messageFor(e));
                return;
            }
            if (!result->isValid()) {
                emit error(tr("The server isn't an OpenVireo account server."));
                return;
            }
            if (const auto insecure = result->insecureUrl(); !insecure.isEmpty()) {
                emit error(insecureServerMessage(insecure));
                return;
            }
            setServer(*result);
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

AccountPlan::Plan AccountManager::plan() const
{
    return m_plan;
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
    if (m_serviceAccount || m_accountId.isEmpty()) {
        return false;
    }
    if (AccountOfflineCache::hasProfile(m_accountId)) {
        return true;
    }
    // The profile the core cached at the last sign-in or unlock (local, no network)
    try {
        return m_core && m_core->account && AccountOfflineCache::isEnabled() && m_core->acct().cached_kdf().has;
    } catch (const std::exception&) {
        return false;
    }
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
    if (!isQuickUnlockAvailable() || m_accountId.isEmpty() || auk.empty()) {
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
        setState(m_accountId.isEmpty() ? State::SignedOut : State::Locked);
    }
    emit error(message);
}

QString AccountManager::defaultServerId() const
{
    return m_config.serverId();
}

QString AccountManager::storedSecretKey(const QString& serverId, const QString& email)
{
    // Where the core keeps it (CoreHost::SecretStore): secret_key:<idS>:<idU>, else the older secret_key:<idU>
    const auto idU = email.trimmed().toLower();
    for (const auto& key : {"secret_key:" + serverId + ":" + idU, "secret_key:" + idU}) {
        const auto value = SyncTokenStore::load(CoreHost::SecretStore::KeyPrefix + key);
        if (!value.isEmpty()) {
            return QString::fromUtf8(QByteArray::fromBase64(value.toLatin1()));
        }
    }
    return {};
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
        account.current = account.id == m_accountId && !m_serviceAccount;
        if (account.current && m_state != State::SignedOut) {
            account.signedIn = true;
        } else if (m_local && !account.id.isEmpty()) {
            // Active or set aside in the keychain (no network)
            try {
                const auto issuer = account.server.issuer.isEmpty() ? account.server.api : account.server.issuer;
                account.signedIn =
                    m_local->client->session_state(rs(issuer), rs(account.id)) != ov::SessionState::SignedOut;
            } catch (const std::exception&) {
                account.signedIn = false;
            }
        }
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

QString AccountManager::unreachableMessage() const
{
    const auto host = hostOf(m_config.issuer.isEmpty() ? m_config.api : m_config.issuer);
    if (AccountOfflineCache::isEnabled()) {
        return tr("Can't reach %1, and this device hasn't kept a copy to unlock offline yet. Check your "
                  "connection and try again.")
            .arg(host);
    }
    return tr("Can't reach %1. Check your connection and try again.").arg(host);
}

QString AccountManager::messageFor(const CoreError& error) const
{
    if (error.code == "network_error") {
        return tr("Can't reach %1. Check your connection and try again.")
            .arg(hostOf(m_config.issuer.isEmpty() ? m_config.api : m_config.issuer));
    }
    if (error.code == "plan_limit") {
        const auto details = QJsonDocument::fromJson(error.details.toUtf8()).object();
        const auto url = details.value("upgrade_url").toString();
        auto message = error.message;
        if (!url.isEmpty()) {
            if (!message.endsWith('.')) {
                message += '.';
            }
            message += ' ' + tr("Change your plan at %1").arg(url);
        }
        return message;
    }
    if (error.status > 0) {
        return tr("The account server responded with HTTP %1: %2").arg(error.status).arg(error.message);
    }
    return error.message.isEmpty() ? error.code : error.message;
}

bool AccountManager::sessionEndedBy(const CoreError& error)
{
    // The core refused refresh (401/403/410) and dropped the session already
    if (error.code != "signed_out" || error.status == 0 || m_serviceAccount) {
        return false;
    }
    const auto reason = QJsonDocument::fromJson(error.details.toUtf8()).object().value("reason").toString();
    endSession(reason == "token_reused"
                   ? tr("Your session was ended because its sign-in token was used twice. Sign in again.")
                   : tr("Your session has ended. Sign in again."));
    return true;
}

// ---------------------------------------------------------------------------------- sign-up and sign-in

void AccountManager::registerAccount(const QString& email, const QString& password)
{
    if (!m_config.isValid() || !m_core) {
        emit error(tr("Choose an account server first."));
        return;
    }
    m_pending = std::make_unique<Pending>();
    m_pending->registering = true;
    m_pending->email = email.trimmed();
    setState(State::SigningIn);

    ov::RegisterInput input;
    input.email = rs(m_pending->email);
    input.password = secretOf(password);
    if (!m_newAccountKdf.salt.isEmpty()) {
        input.kdf = ov::KdfOverrides{true,
                                     m_newAccountKdf.memoryKiB,
                                     true,
                                     m_newAccountKdf.iterations,
                                     true,
                                     m_newAccountKdf.parallelism};
    }
    input.trust_device = true;
    auto shared = std::make_shared<ov::RegisterInput>(std::move(input));
    auto info = std::make_shared<SessionInfo>();
    auto core = m_core;
    runCore(
        core,
        [shared, info](CoreSession& c) {
            auto created = c.acct().register_account(std::move(*shared), *ov::new_cancel_token());
            info->secretKey = takeText(created.secret_key);
            info->auk = takeSecure(created.auk);
            readSession(c.acct(), *info, true);
        },
        [this, info, core](const CoreError& e) {
            if (!m_pending || core != m_core) {
                return;
            }
            if (!e.ok()) {
                fail(e.code == "conflict" ? tr("An account with this email address already exists.")
                     : e.code == "gone"   ? tr("The registration took too long. Try again.")
                                          : messageFor(e));
                return;
            }
            finishSignIn(*info, true);
        });
}

void AccountManager::signIn(const QString& email, const QString& password, const QString& secretKey)
{
    if (!m_config.isValid() || !m_core) {
        emit error(tr("Choose an account server first."));
        return;
    }
    m_pending = std::make_unique<Pending>();
    m_pending->email = email.trimmed();
    const bool typed = !secretKey.trimmed().isEmpty();
    if (!typed && storedSecretKey(defaultServerId(), m_pending->email).isEmpty()) {
        fail(tr("Enter your Secret Key: this device hasn't signed in to this account before."));
        return;
    }
    SecureBytes parsed;
    if (typed && !parseSecretKey(secretKey, &parsed)) {
        fail(tr("The Secret Key isn't valid. It looks like K1-XXXXXX-XXXXX-XXXXX-XXXXX-XXXXX."));
        return;
    }
    setState(State::SigningIn);

    ov::LoginInput input;
    input.email = rs(m_pending->email);
    input.password = secretOf(password);
    input.has_secret_key = typed;
    if (typed) {
        input.secret_key = secretOf(formatSecretKey(parsed));
    }
    input.trust_device = true;
    auto shared = std::make_shared<ov::LoginInput>(std::move(input));
    auto info = std::make_shared<SessionInfo>();
    auto core = m_core;
    runCore(
        core,
        [shared, info](CoreSession& c) {
            auto step = c.acct().login(std::move(*shared), *ov::new_cancel_token());
            if (step->needs_totp()) {
                info->mfa = true;
                info->challenge = std::make_shared<rust::Box<ov::TotpChallenge>>(step->challenge());
                return;
            }
            auto unlocked = step->take_unlocked();
            info->auk = takeSecure(unlocked.auk);
            info->deletion = qs(unlocked.deletion_scheduled_for);
            readSession(c.acct(), *info, true);
        },
        [this, info, core](const CoreError& e) {
            if (!m_pending || core != m_core) {
                return;
            }
            if (!e.ok()) {
                // A wrong password (or an unknown email) fails OPAQUE on this device: unauthorized either way
                fail(wrongSecretKey(e)          ? tr("The Secret Key is wrong.")
                     : wrongKey(e)              ? tr("The Secret Key or the master password is wrong.")
                     : e.code == "unauthorized" ? tr("The email address or the master password is wrong.")
                                                : messageFor(e));
                return;
            }
            if (info->mfa) {
                m_pending->challenge = info->challenge;
                setState(State::NeedsMfa);
                emit mfaRequired({"totp"});
                return;
            }
            finishSignIn(*info, false);
        });
}

void AccountManager::submitMfa(const QString& totp)
{
    if (m_state != State::NeedsMfa || !m_pending || !m_pending->challenge) {
        return;
    }
    setState(State::SigningIn);
    auto challenge = m_pending->challenge;
    auto info = std::make_shared<SessionInfo>();
    auto core = m_core;
    const auto code = totp;
    runCore(
        core,
        [challenge, info, code](CoreSession& c) {
            auto step = c.acct().submit_totp(**challenge, rs(code), *ov::new_cancel_token());
            auto unlocked = step->take_unlocked();
            info->auk = takeSecure(unlocked.auk);
            info->deletion = qs(unlocked.deletion_scheduled_for);
            readSession(c.acct(), *info, true);
        },
        [this, info, core](const CoreError& e) {
            if (!m_pending || core != m_core) {
                return;
            }
            if (e.code == "invalid_mfa") {
                // Let the user try another code; after the 5th wrong one the server drops the token
                setState(State::NeedsMfa);
                emit error(tr("The code is wrong or has expired."));
                return;
            }
            if (!e.ok()) {
                fail(e.code == "mfa_expired" ? tr("The sign-in expired or the code was wrong too often. Sign in again.")
                     : wrongSecretKey(e)    ? tr("The Secret Key is wrong.")
                     : wrongKey(e)          ? tr("The Secret Key or the master password is wrong.")
                                            : messageFor(e));
                return;
            }
            finishSignIn(*info, false);
        });
}

void AccountManager::finishSignIn(const SessionInfo& info, bool registering)
{
    m_pending.reset();
    m_serviceAccount = false;
    m_accountId = info.userId;
    m_email = info.email;
    m_deviceId = info.deviceId;
    m_keyset = keysetOf(info.keysetSecret);
    takeToken(info.accessToken);
    // From now on Quick Unlock can stand in for the master password
    rememberAuk(info.auk);
    SyncTokenStore::save(LastAccountKey, m_accountId);
    rememberCurrentAccount();
    emit knownAccountsChanged();
    registerAccountHooks();
    setOffline(false);
    m_deletionScheduledFor.clear();
    setState(State::Unlocked);
    if (!info.deletion.isEmpty()) {
        setPendingDeletion(info.deletion);
    }
    if (registering) {
        emit registered(info.secretKey);
    }
    emit signedIn();
    reloadVaults();
    startEvents();
}

// ------------------------------------------------------------------------ sessions on this device (§21)

bool AccountManager::restoreSession(const QString& accountId)
{
    if (accountId.isEmpty()) {
        return false;
    }
    KnownAccount known;
    for (const auto& account : knownAccounts()) {
        if (account.id == accountId) {
            known = account;
        }
    }
    if (known.id.isEmpty() || !known.server.isValid()) {
        return false;
    }
    setServer(known.server);
    if (m_state == State::SignedOut && m_inFlight == 0) {
        // Read the keychain afresh: the core reads its session once per account object
        m_core = makeCore(known.server);
    }
    if (!m_core) {
        return false;
    }
    QString email;
    QString deviceId;
    try {
        const auto& account = m_core->acct();
        auto stored = account.stored_session();
        if (!stored.has || qs(stored.value.user_id) != accountId) {
            if (stored.has) {
                // Another account's session is the active one: set it aside first
                account.park_session();
            }
            if (!account.resume_session(rs(accountId))) {
                return false;
            }
            stored = account.stored_session();
        }
        email = qs(stored.value.email);
        deviceId = qs(stored.value.device_id);
    } catch (const std::exception& e) {
        qWarning("Restoring the account session failed: %s", e.what());
        return false;
    }
    m_serviceAccount = false;
    m_accountId = accountId;
    m_email = email.isEmpty() ? known.email : email;
    m_deviceId = deviceId;
    m_accessToken.clear();
    m_accessTokenExpiry = 0;
    registerAccountHooks();
    rememberCurrentAccount();
    setState(State::Locked);
    emit knownAccountsChanged();
    return true;
}

void AccountManager::setAside(std::function<void()> done)
{
    if (m_serviceAccount || m_accountId.isEmpty() || !m_core) {
        done();
        return;
    }
    const auto finish = [this, done] {
        stopEvents();
        m_refreshTimer.stop();
        unregisterAccountHooks();
        m_pending.reset();
        m_keyring.clear();
        m_keyset.clear();
        m_orgs.clear();
        m_plan = {};
        setOffline(false);
        m_accessToken.clear();
        m_accessTokenExpiry = 0;
        m_accountId.clear();
        m_email.clear();
        m_deviceId.clear();
        m_deletionScheduledFor.clear();
        setState(State::SignedOut);
        emit vaultsChanged();
        emit knownAccountsChanged();
        done();
    };
    if (m_inFlight == 0) {
        // Nothing is rotating a token right now: the keychain move is quick
        try {
            m_core->acct().park_session();
        } catch (const std::exception& e) {
            qWarning("Setting the account aside failed: %s", e.what());
        }
        finish();
        return;
    }
    // A refresh under way finishes first: the worker runs calls in order
    QPointer<AccountManager> guard(this);
    runCore(
        m_core, [](CoreSession& c) { c.acct().park_session(); }, [guard, finish](const CoreError&) {
            if (guard) {
                finish();
            }
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
        if (account.id != accountId || account.current) {
            continue;
        }
        auto* provider = m_quickUnlock ? m_quickUnlock : getQuickUnlock();
        provider->reset(quickUnlockIdOf(account.server.serverId(), accountId));
        AccountOfflineCache::clear(accountId);
        const auto id = accountId;
        QPointer<AccountManager> guard(this);
        runCore(
            makeCore(account.server),
            [id](CoreSession& c) { c.acct().sign_out_parked(rs(id), *ov::new_cancel_token()); },
            [guard](const CoreError&) {
                if (guard) {
                    emit guard->knownAccountsChanged();
                }
            });
    }
    emit knownAccountsChanged();
}

void AccountManager::forgetKnown(const QString& accountId)
{
    auto list = knownAccounts();
    for (const auto& account : list) {
        if (account.id != accountId || account.current) {
            continue;
        }
        signOutKnown(accountId);
        const auto email = account.email;
        QPointer<AccountManager> guard(this);
        // After the sign-out above (calls run in order): the Secret Key and device id go too
        runCore(
            makeCore(account.server),
            [email](CoreSession& c) { c.acct().forget_device(rs(email)); },
            [guard](const CoreError&) {
                if (guard) {
                    emit guard->knownAccountsChanged();
                }
            });
        list.removeIf([&](const KnownAccount& other) { return other.id == accountId; });
        saveKnownAccounts(list);
        break;
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
        auto result = std::make_shared<ov::ParkedCheck>(ov::ParkedCheck::NoAnswer);
        QPointer<AccountManager> guard(this);
        // A refresh keeps the session within its 30 days; a refusal ends it, and only it (SPEC §20, §21)
        runCore(
            makeCore(account.server),
            [id, result](CoreSession& c) { *result = c.acct().check_parked_session(rs(id), *ov::new_cancel_token()); },
            [guard, id, result](const CoreError& e) {
                if (!guard || !e.ok()) {
                    return;
                }
                if (*result == ov::ParkedCheck::Valid) {
                    auto list = guard->knownAccounts();
                    for (auto& known : list) {
                        if (known.id == id) {
                            known.checkedAt = now();
                        }
                    }
                    saveKnownAccounts(list);
                } else if (*result == ov::ParkedCheck::Ended) {
                    guard->signOutKnown(id);
                }
                emit guard->knownAccountsChanged();
            });
    }
}

// ------------------------------------------------------------------------------------- unlock and lock

void AccountManager::unlock(const QString& password)
{
    if (m_state != State::Locked || m_serviceAccount || !m_core) {
        return;
    }
    if (storedSecretKey(defaultServerId(), m_email).isEmpty()) {
        m_pending.reset();
        emit error(tr("This device no longer has the account's Secret Key. Sign in again."));
        return;
    }
    m_pending = std::make_unique<Pending>();
    m_pending->email = m_email;
    setState(State::SigningIn);
    auto pw = std::make_shared<rust::Vec<uint8_t>>(secretOf(password));
    auto info = std::make_shared<SessionInfo>();
    auto core = m_core;
    // The keyset and its KDF settings come from the server; when it can't be reached, the copy the core kept
    // at the last unlock stands in (SPEC §20)
    runCore(
        core,
        [pw, info](CoreSession& c) {
            auto unlocked = c.acct().unlock(std::move(*pw), {}, *ov::new_cancel_token());
            info->auk = takeSecure(unlocked.auk);
            info->offline = unlocked.offline;
            info->deletion = qs(unlocked.deletion_scheduled_for);
            readSession(c.acct(), *info, !unlocked.offline);
            if (unlocked.offline) {
                loadVaultList(c.acct(), info->vaults, true);
            }
        },
        [this, info, core](const CoreError& e) {
            if (!m_pending || core != m_core) {
                return;
            }
            m_pending.reset();
            if (!e.ok()) {
                if (sessionEndedBy(e)) {
                    return;
                }
                setState(State::Locked);
                emit error(e.code == "network_error" ? unreachableMessage()
                           : wrongKey(e)
                               ? tr("The Secret Key or the master password is wrong.")
                               : messageFor(e));
                return;
            }
            m_keyset = keysetOf(info->keysetSecret);
            if (!info->accessToken.isEmpty()) {
                takeToken(info->accessToken);
            }
            rememberAuk(info->auk);
            followEmail(info->email);
            if (!info->offline) {
                setPendingDeletion(info->deletion);
            }
            if (info->offline) {
                m_keyring.setVaults(info->vaults.vaults, info->vaults.keys);
            }
            finishUnlock(info->offline);
        });
}

void AccountManager::quickUnlock()
{
    if (!canQuickUnlock() || !m_core) {
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
    auto auk = std::make_shared<rust::Vec<uint8_t>>(bytesOf(key));
    Botan::secure_scrub_memory(key.data(), key.size());
    m_pending = std::make_unique<Pending>();
    m_pending->email = m_email;
    setState(State::SigningIn);
    auto info = std::make_shared<SessionInfo>();
    auto core = m_core;
    runCore(
        core,
        [auk, info](CoreSession& c) {
            c.acct().unlock_with_auk(std::move(*auk), *ov::new_cancel_token());
            info->offline = c.acct().is_offline();
            info->deletion = qs(c.acct().deletion_scheduled_for());
            readSession(c.acct(), *info, !info->offline);
            if (info->offline) {
                loadVaultList(c.acct(), info->vaults, true);
            }
        },
        [this, info, core](const CoreError& e) {
            if (!m_pending || core != m_core) {
                return;
            }
            m_pending.reset();
            if (!e.ok()) {
                if (sessionEndedBy(e)) {
                    return;
                }
                if (wrongKey(e)) {
                    // The keyset was wrapped again, e.g. the master password changed on another device
                    forgetAuk();
                }
                setState(State::Locked);
                if (e.code == "network_error") {
                    emit error(unreachableMessage());
                } else if (wrongKey(e)) {
                    emit error(tr("Quick Unlock no longer opens this account, e.g. because its master password "
                                  "changed. Unlock it with the master password."));
                } else {
                    emit error(messageFor(e));
                }
                return;
            }
            m_keyset = keysetOf(info->keysetSecret);
            if (!info->accessToken.isEmpty()) {
                takeToken(info->accessToken);
            }
            followEmail(info->email);
            if (!info->offline) {
                setPendingDeletion(info->deletion);
            }
            if (info->offline) {
                m_keyring.setVaults(info->vaults.vaults, info->vaults.keys);
            }
            finishUnlock(info->offline);
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
    // The vaults as the core last listed them (the unlock read them into the keyring); nothing goes out until the
    // servers answer again
    const auto cache = AccountOfflineCache::load(m_accountId);
    m_orgs = parseOrgs(cache.value("orgs").toArray());
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
    m_plan = AccountPlan::parse(me.value("plan"));
    setPendingDeletion(me.value("deletion_scheduled_for").toString());
    AccountOfflineCache::saveProfile(
        m_accountId, me.value("kdf").toObject(), me.value("keyset").toObject(), me.value("orgs").toArray());
}

void AccountManager::lock()
{
    setOffline(false);
    m_pending.reset();
    m_keyring.clear();
    m_keyset.clear();
    m_orgs.clear();
    m_plan = {};
    if (m_core && m_core->account) {
        m_core->acct().lock();
    }
    if (m_state == State::Unlocked || m_state == State::SigningIn || m_state == State::NeedsMfa) {
        if (m_serviceAccount) {
            // A service account has no session to keep without its token
            endSession({});
            return;
        }
        setState(m_accountId.isEmpty() ? State::SignedOut : State::Locked);
        emit vaultsChanged();
    }
}

void AccountManager::signOut()
{
    // The core tells the server (best effort) and drops the session, the offline copies with it
    endSession({}, m_serviceAccount ? CoreEnd::None : CoreEnd::SignOut);
}

void AccountManager::endSession(const QString& reason, CoreEnd end)
{
    stopEvents();
    m_refreshTimer.stop();
    m_pending.reset();
    // Like closing a database: its Quick Unlock goes with the session
    forgetAuk();
    m_orgs.clear();
    m_plan = {};
    setOffline(false);
    if (!m_accountId.isEmpty()) {
        unregisterAccountHooks();
        if (!m_serviceAccount) {
            // The offline cache goes with the session (SPEC §20)
            AccountOfflineCache::clear(m_accountId);
        }
    }
    if (end != CoreEnd::None && m_core && m_core->account) {
        const bool forget = end == CoreEnd::Forget;
        m_core->acct().lock();
        runCore(
            m_core,
            [forget](CoreSession& c) { c.acct().sign_out(forget, *ov::new_cancel_token()); },
            [](const CoreError& e) {
                if (!e.ok()) {
                    qWarning("Signing out of the account: %s", qPrintable(e.message));
                }
            });
    }
    m_accessToken.clear();
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
    // The core moved the Secret Key and device id to the new email already (SPEC §16)
    if (email.isEmpty() || m_serviceAccount || email.trimmed().toLower() == m_email.trimmed().toLower()) {
        return;
    }
    m_email = email;
    rememberCurrentAccount();
    emit emailChanged();
    emit knownAccountsChanged();
}

// ------------------------------------------------------------------------------------------ tokens

void AccountManager::takeToken(const QString& token)
{
    if (token.isEmpty()) {
        return;
    }
    const bool changed = token != m_accessToken;
    m_accessToken = token;
    const auto lifetime = lifetimeOf(token);
    m_accessTokenExpiry = now() + lifetime;
    m_refreshTimer.start(qMax<qint64>(1, lifetime - RefreshMarginSec) * 1000);
    if (changed) {
        emit accessTokenChanged();
    }
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
    if (m_refreshing && !m_serviceAccount) {
        // A refresh is under way (new vlt claims after an access change, say): its token, not the old one
        m_tokenWaiters << done;
        return;
    }
    if (!m_accessToken.isEmpty() && m_accessTokenExpiry - now() > MinTokenLifetimeSec) {
        done(m_accessToken);
        return;
    }
    if (m_serviceAccount) {
        if (m_saToken.secret.empty()) {
            done({});
            return;
        }
        m_tokenWaiters << done;
        exchangeServiceAccount();
        return;
    }
    if (m_accountId.isEmpty() || !m_core) {
        done({});
        return;
    }
    m_tokenWaiters << done;
    if (m_refreshing) {
        return;
    }
    m_refreshing = true;
    auto token = std::make_shared<QString>();
    auto core = m_core;
    runCore(
        core,
        [token](CoreSession& c) {
            auto bytes = c.acct().access_token(false, *ov::new_cancel_token());
            *token = takeText(bytes);
        },
        [this, token, core](const CoreError& e) {
            if (core != m_core) {
                return;
            }
            m_refreshing = false;
            if (e.ok()) {
                takeToken(*token);
            }
            finishTokenRequest(e);
        });
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
    if (m_accountId.isEmpty() || !m_core) {
        finishRefresh(false);
        return;
    }
    m_refreshing = true;
    auto token = std::make_shared<QString>();
    auto core = m_core;
    runCore(
        core,
        [token](CoreSession& c) {
            auto bytes = c.acct().access_token(true, *ov::new_cancel_token());
            *token = takeText(bytes);
        },
        [this, token, core](const CoreError& e) {
            if (core != m_core) {
                return;
            }
            m_refreshing = false;
            if (e.ok()) {
                takeToken(*token);
            }
            finishTokenRequest(e);
        });
}

void AccountManager::finishTokenRequest(const CoreError& e)
{
    if (e.ok()) {
        finishRefresh(true);
        return;
    }
    // Revoked, expired or reused (token_reused): the session is over on the server
    if (sessionEndedBy(e)) {
        return;
    }
    // The network or the server is down: keep the session and try again later
    m_refreshTimer.start(30 * 1000);
    finishRefresh(false);
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

// ----------------------------------------------------------------------------------- service accounts

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
            const auto token = json.value("access_token").toString();
            const auto expiresIn = json.value("expires_in").toInt();
            const bool changed = token != m_accessToken;
            m_accessToken = token;
            m_accessTokenExpiry = now() + (expiresIn > 0 ? expiresIn : lifetimeOf(token));
            m_refreshTimer.start(qMax<qint64>(1, m_accessTokenExpiry - now() - RefreshMarginSec) * 1000);
            if (changed) {
                emit accessTokenChanged();
            }
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

// --------------------------------------------------------------------------------------------- vaults

void AccountManager::reloadVaults()
{
    if (m_state != State::Unlocked) {
        return;
    }
    if (m_serviceAccount) {
        // Service accounts sign in through AuthClient for now: their vault keys come from GET /vaults directly
        m_client->listVaults([this](const AuthClient::Response& response) {
            if (m_state != State::Unlocked || !m_serviceAccount) {
                return;
            }
            if (!response.isSuccess()) {
                emit error(AuthClient::errorMessage(response));
                return;
            }
            m_keyring.setVaults(response.json().value("vaults").toArray(), m_keyset.encSk);
            emit vaultsChanged();
        });
        return;
    }
    auto list = std::make_shared<VaultList>();
    auto core = m_core;
    // GET /vaults in the core: it decrypts the names, unseals the keys and keeps the list for offline unlock
    runCore(
        core,
        [list](CoreSession& c) { loadVaultList(c.acct(), *list, false); },
        [this, list, core](const CoreError& e) {
            if (core != m_core || m_state != State::Unlocked || m_serviceAccount) {
                return;
            }
            if (!e.ok()) {
                if (sessionEndedBy(e)) {
                    return;
                }
                if (e.code == "network_error" || e.status == 502 || e.status == 503 || e.status == 504) {
                    // Work from what is open and cached until the servers answer again
                    if (m_offline) {
                        scheduleReconnect();
                    } else {
                        setOffline(true);
                    }
                    return;
                }
                if (e.serverCode == "account_pending_deletion") {
                    // SPEC §23: nothing opens until the account is restored; GET /me brings the date
                    const auto details = QJsonDocument::fromJson(e.details.toUtf8()).object();
                    const auto when = details.value("scheduled_for").toString();
                    if (!when.isEmpty()) {
                        setPendingDeletion(when);
                    }
                    return;
                }
                emit error(messageFor(e));
                return;
            }
            if (list->offline) {
                // The core fell back to its cached list: keep what is open until the servers answer again
                if (m_offline) {
                    scheduleReconnect();
                } else {
                    setOffline(true);
                }
                return;
            }
            takeVaultList(*list);
        });
    reloadOrgs();
}

void AccountManager::takeVaultList(const VaultList& list)
{
    m_keyring.setVaults(list.vaults, list.keys);
    if (m_offline) {
        setOffline(false);
        startEvents();
    }
    emit vaultsChanged();
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
        endSession(tr("This account is scheduled for deletion. Sign in to restore it."), CoreEnd::SignOut);
        return;
    }
    if (event.change == "account_deleted") {
        auto list = knownAccounts();
        const auto id = m_accountId;
        list.removeIf([id](const KnownAccount& a) { return a.id == id; });
        saveKnownAccounts(list);
        // Erased: the Secret Key and device id leave this device with the session
        endSession(tr("This account was deleted."), CoreEnd::Forget);
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
    createVault(name, {}, std::move(done));
}

void AccountManager::createVault(const QString& name, const QString& orgId, CreateVaultCallback done)
{
    if (m_state != State::Unlocked || m_serviceAccount || !m_core) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    struct Created
    {
        QString vaultId;
        QByteArray kdbx;
        QByteArray keyFile;
        VaultList list;
    };
    auto created = std::make_shared<Created>();
    auto core = m_core;
    QPointer<AccountManager> guard(this);
    // The core seals a new vault key to this account (and an org's owners and admins), uploads the empty vault with
    // If-None-Match: *, and refreshes the token for the new vlt claim; the window keeps it as a KDBX file
    runCore(
        core,
        [created, name, orgId](CoreSession& c) {
            auto cancel = ov::new_cancel_token();
            auto vault = c.acct().create_vault(rs(name), rs(orgId), *cancel);
            created->vaultId = qs(vault->id());
            auto credentials = c.acct().vault_kdbx_credentials(rs(created->vaultId), *cancel);
            created->keyFile = toBytes(credentials.key_file);
            auto kdbx = vault->export_kdbx(std::move(credentials), {}, *cancel);
            created->kdbx = toBytes(kdbx);
            ov::wipe(kdbx);
            loadVaultList(c.acct(), created->list, false);
        },
        [guard, created, core, done](const CoreError& e) {
            const auto scrub = qScopeGuard([created] {
                Botan::secure_scrub_memory(created->keyFile.data(), static_cast<size_t>(created->keyFile.size()));
            });
            if (!guard) {
                return;
            }
            if (!e.ok()) {
                if (guard->sessionEndedBy(e)) {
                    done({}, {}, tr("Your session has ended. Sign in again."));
                    return;
                }
                done({}, {}, guard->messageFor(e));
                return;
            }
            QByteArray data(created->kdbx);
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            auto db = QSharedPointer<Database>::create();
            KeePass2Reader reader;
            if (!reader.readDatabase(&buffer, vaultCompositeKey(created->keyFile), db.data())) {
                done({}, {}, reader.errorString());
                return;
            }
            if (core == guard->m_core && guard->m_state == State::Unlocked) {
                guard->takeVaultList(created->list);
                // The core refreshed its token; hand out the new one from now on
                guard->m_accessTokenExpiry = 0;
            }
            done(created->vaultId, db, {});
        });
}

void AccountManager::downloadVault(const QString& vaultId, DownloadVaultCallback done)
{
    if (m_state != State::Unlocked || m_serviceAccount || !m_core) {
        done({}, QStringLiteral("signed_out"), tr("Unlock your account first."));
        return;
    }
    auto kdbx = std::make_shared<QByteArray>();
    auto keyFile = std::make_shared<QByteArray>();
    QPointer<AccountManager> guard(this);
    runCore(
        m_core,
        [vaultId, kdbx, keyFile](CoreSession& c) {
            auto cancel = ov::new_cancel_token();
            auto vault = c.acct().open_vault(rs(vaultId), *cancel);
            auto credentials = c.acct().vault_kdbx_credentials(rs(vaultId), *cancel);
            *keyFile = toBytes(credentials.key_file);
            auto data = vault->export_kdbx(std::move(credentials), {}, *cancel);
            *kdbx = toBytes(data);
            ov::wipe(data);
        },
        [guard, kdbx, keyFile, done](const CoreError& e) {
            const auto scrub = qScopeGuard([keyFile] {
                Botan::secure_scrub_memory(keyFile->data(), static_cast<size_t>(keyFile->size()));
            });
            if (!guard) {
                return;
            }
            if (!e.ok()) {
                if (guard->sessionEndedBy(e)) {
                    done({}, e.code, tr("Your session has ended. Sign in again."));
                    return;
                }
                done({}, e.code, guard->messageFor(e));
                return;
            }
            QBuffer buffer(kdbx.get());
            buffer.open(QIODevice::ReadOnly);
            auto db = QSharedPointer<Database>::create();
            KeePass2Reader reader;
            if (!reader.readDatabase(&buffer, vaultCompositeKey(*keyFile), db.data())) {
                done({}, QStringLiteral("kdbx_error"), reader.errorString());
                return;
            }
            done(db, {}, {});
        });
}

AccountVaultSync::Cancel AccountManager::syncVault(const AccountVaultSync::Request& request,
                                                   AccountVaultSync::Done done)
{
    auto cancel = std::make_shared<rust::Box<ov::CancelToken>>(ov::new_cancel_token());
    auto result = std::make_shared<AccountVaultSync::Result>();
    auto core = m_core;
    if (m_state != State::Unlocked || m_serviceAccount || !core) {
        result->errorCode = QStringLiteral("signed_out");
        result->error = tr("Unlock your account first.");
        QTimer::singleShot(0, this, [result, done] { done(*result); });
        return [] {};
    }
    auto req = std::make_shared<AccountVaultSync::Request>(request);
    QPointer<AccountManager> guard(this);
    runCore(
        core,
        [req, result, cancel](CoreSession& c) {
            ov::SyncOptions options;
            options.local_changed = req->localChanged;
            options.accept_server_state = req->acceptServerState;
            auto synced = c.acct().sync_vault_kdbx(rs(req->vaultId),
                                                   bytesOf(req->kdbx),
                                                   !req->base.isEmpty(),
                                                   bytesOf(req->base),
                                                   options,
                                                   **cancel);
            result->kdbx = toBytes(synced.kdbx);
            ov::wipe(synced.kdbx);
            result->keyFile = toBytes(synced.credentials.key_file);
            ov::wipe(synced.credentials.key_file);
            ov::wipe(synced.credentials.password);
            const auto& report = synced.report;
            switch (report.outcome) {
            case ov::SyncOutcome::Pulled:
                result->outcome = AccountVaultSync::Outcome::Pulled;
                break;
            case ov::SyncOutcome::Uploaded:
                result->outcome = AccountVaultSync::Outcome::Uploaded;
                break;
            case ov::SyncOutcome::Created:
                result->outcome = AccountVaultSync::Outcome::Created;
                break;
            default:
                result->outcome = AccountVaultSync::Outcome::Unchanged;
                break;
            }
            result->etag = qs(report.etag);
            result->hasClockOffset = report.has_clock_offset;
            result->clockOffsetMsec = report.clock_offset_ms;
            for (const auto& conflict : report.conflicts) {
                AccountVaultSync::Conflict out;
                out.uuid = qs(conflict.uuid);
                out.kind = conflict.kind == ov::ConflictKind::DeletedHereEditedThere   ? 1
                           : conflict.kind == ov::ConflictKind::EditedHereDeletedThere ? 2
                                                                                        : 0;
                out.title = qs(conflict.title);
                for (const auto& field : conflict.fields) {
                    out.fields << qs(field);
                }
                result->conflicts << out;
            }
        },
        [guard, result, core, done](const CoreError& e) {
            if (!e.ok()) {
                result->errorCode = e.code;
                result->error = guard ? guard->messageFor(e) : e.message;
                if (e.code == "server_rolled_back" || e.code == "server_lost_database") {
                    result->error = e.message;
                }
                if (guard && core == guard->m_core && guard->sessionEndedBy(e)) {
                    result->error = tr("Your session has ended. Sign in again.");
                }
            }
            done(*result);
            Botan::secure_scrub_memory(result->keyFile.data(), static_cast<size_t>(result->keyFile.size()));
        });
    return [cancel] { (*cancel)->cancel(); };
}

QSharedPointer<CompositeKey> AccountManager::vaultKey(const QString& vaultId) const
{
    return m_keyring.compositeKey(vaultId);
}

QSharedPointer<CompositeKey> AccountManager::vaultCompositeKey(const QByteArray& keyFile)
{
    if (keyFile.size() != AccountCrypto::KeySize) {
        return {};
    }
    // The vault key is a raw 32-byte key file (SPEC §7)
    return VaultKeyring::compositeKeyFor(SecureBytes(keyFile.begin(), keyFile.end()));
}

void AccountManager::registerAccountHooks()
{
    SyncServerClient::setAccountTokenProvider(m_accountId, tokenProvider());
    if (m_serviceAccount) {
        return;
    }
    QPointer<AccountManager> guard(this);
    SyncServerClient::setAccountVaultSyncer(
        m_accountId,
        [guard](const AccountVaultSync::Request& request, AccountVaultSync::Done done) -> AccountVaultSync::Cancel {
            if (!guard) {
                AccountVaultSync::Result result;
                result.errorCode = QStringLiteral("signed_out");
                result.error = tr("Sign in to your OpenVireo account to sync this vault.");
                done(result);
                return [] {};
            }
            return guard->syncVault(request, std::move(done));
        });
}

void AccountManager::unregisterAccountHooks()
{
    SyncServerClient::setAccountTokenProvider(m_accountId, nullptr);
    SyncServerClient::setAccountVaultSyncer(m_accountId, nullptr);
}

void AccountManager::startEvents()
{
    if (!m_eventsEnabled || m_events || m_config.sync.isEmpty() || m_state != State::Unlocked || !m_core) {
        return;
    }
    if (m_serviceAccount) {
        return;
    }
    auto loop = std::make_shared<EventLoop>();
    loop->cancel = ov::new_cancel_token();
    auto core = m_core;
    QPointer<AccountManager> guard(this);
    m_events = loop;
    loop->thread = std::thread([loop, core, guard] {
        try {
            auto stream = core->acct().subscribe(**loop->cancel);
            const ov::EventStream* reader = nullptr;
            {
                std::lock_guard<std::mutex> lock(loop->mutex);
                loop->stream = std::move(stream);
                reader = &**loop->stream;
            }
            while (!loop->stop) {
                auto event = std::make_shared<std::optional<ov::AccountEvent>>();
                try {
                    *event = reader->next(**loop->cancel);
                } catch (const std::exception&) {
                    // A connection error while reconnecting; the stream keeps trying
                    if (loop->stop) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                if ((*event)->kind == ov::AccountEventKind::Closed) {
                    break;
                }
                QMetaObject::invokeMethod(
                    qApp,
                    [guard, event, loop] {
                        if (guard && guard->m_events == loop) {
                            guard->takeCoreEvent(**event);
                        }
                    },
                    Qt::QueuedConnection);
            }
        } catch (const std::exception& e) {
            qWarning("The account's event stream stopped: %s", e.what());
        }
        loop->done = true;
    });
}

void AccountManager::stopEvents()
{
    auto loop = std::move(m_events);
    m_events.reset();
    if (!loop) {
        return;
    }
    loop->stop = true;
    {
        std::lock_guard<std::mutex> lock(loop->mutex);
        if (loop->cancel) {
            (*loop->cancel)->cancel();
        }
        if (loop->stream) {
            (*loop->stream)->close();
        }
    }
    // The cancelled call returns at once; a test Host may need this thread's event loop to answer meanwhile
    for (int i = 0; i < 40 && !loop->done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (!loop->done) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
    }
    loop->thread.join();
}

void AccountManager::takeCoreEvent(const ov::AccountEvent& event)
{
    switch (event.kind) {
    case ov::AccountEventKind::VaultChanged:
    case ov::AccountEventKind::DatabaseChanged: {
        SyncEventStream::Event changed;
        changed.type = SyncEventStream::Event::Type::Database;
        changed.databaseId = qs(event.id);
        changed.etag = qs(event.etag);
        changed.revision = event.has_revision ? static_cast<qint64>(event.revision) : -1;
        changed.deleted = event.deleted;
        emit vaultChanged(changed);
        break;
    }
    case ov::AccountEventKind::VaultsChanged:
        // The core refreshed its token for the new vlt claims already: take that token and the vault list
        m_accessTokenExpiry = 0;
        emit accessTokenChanged();
        reloadVaults();
        break;
    case ov::AccountEventKind::SessionEnded: {
        SyncEventStream::Event ended;
        ended.type = SyncEventStream::Event::Type::Access;
        ended.change = event.reason == ov::SignedOutReason::AccountDeleted ? QStringLiteral("account_deleted")
                                                                           : QStringLiteral("deletion_scheduled");
        handleAccessEvent(ended);
        break;
    }
    default:
        break;
    }
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
        AccountOfflineCache::clear(account.id);
        const auto id = account.id;
        const auto email = account.email;
        runCore(
            makeCore(account.server),
            [id, email](CoreSession& c) {
                c.acct().sign_out_parked(rs(id), *ov::new_cancel_token());
                c.acct().forget_device(rs(email));
            },
            {});
        auto list = knownAccounts();
        list.removeIf([&](const KnownAccount& a) { return a.id == account.id; });
        saveKnownAccounts(list);
        changed = true;
    }
    if (changed) {
        emit knownAccountsChanged();
    }
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
    if (m_serviceAccount || m_state != State::Unlocked || !m_core) {
        done({}, tr("Unlock your account first."));
        return;
    }
    auto pw = std::make_shared<rust::Vec<uint8_t>>(secretOf(password));
    auto when = std::make_shared<QString>();
    const auto typed = confirm.trimmed();
    QPointer<AccountManager> guard(this);
    // The core checks the password on this device, gets a fresh token and schedules the deletion; the server ends
    // every session, and the core this one (the Secret Key stays, for a restore)
    runCore(
        m_core,
        [pw, when, typed](CoreSession& c) {
            *when = qs(c.acct().delete_account(std::move(*pw), rs(typed), *ov::new_cancel_token()));
        },
        [guard, when, done](const CoreError& e) {
            if (!guard) {
                return;
            }
            if (!e.ok()) {
                done({}, wrongKey(e)
                             ? tr("That master password is wrong.")
                             : guard->messageFor(e));
                return;
            }
            guard->setKnownDeletion(guard->m_accountId, *when);
            const auto date = QLocale().toString(QDateTime::fromString(*when, Qt::ISODate).toLocalTime().date(),
                                                 QLocale::LongFormat);
            guard->endSession(
                tr("Your account is locked and will be deleted on %1. Sign in before then to restore it.").arg(date));
            done(*when, {});
        });
}

void AccountManager::restoreAccount(const QString& password, std::function<void(const QString& error)> done)
{
    if (m_serviceAccount || m_deletionScheduledFor.isEmpty() || !m_core) {
        done(tr("This account isn't scheduled for deletion."));
        return;
    }
    auto pw = std::make_shared<rust::Vec<uint8_t>>(secretOf(password));
    auto token = std::make_shared<QString>();
    QPointer<AccountManager> guard(this);
    runCore(
        m_core,
        [pw, token](CoreSession& c) {
            c.acct().restore_account(std::move(*pw), *ov::new_cancel_token());
            // The token without `del`, which opens vaults again
            auto bytes = c.acct().access_token(false, *ov::new_cancel_token());
            *token = takeText(bytes);
        },
        [guard, token, done](const CoreError& e) {
            if (!guard) {
                return;
            }
            if (!e.ok()) {
                if (guard->sessionEndedBy(e)) {
                    done(tr("Your session has ended. Sign in again."));
                    return;
                }
                done(wrongKey(e) ? tr("That master password is wrong.")
                                                                                     : guard->messageFor(e));
                return;
            }
            guard->takeToken(*token);
            guard->setPendingDeletion({});
            guard->reloadVaults();
            guard->startEvents();
            done({});
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
