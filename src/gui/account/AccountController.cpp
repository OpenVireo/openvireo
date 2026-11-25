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

#include "AccountController.h"

#include "config-keepassx.h"

#include "account/AccountManager.h"
#include "account/AccountOfflineCache.h"
#include "account/ServiceAccountAdmin.h"
#include "account/VaultKeyring.h"
#include "account/ItemShare.h"
#include "account/VaultSharing.h"
#include "gui/account/DeleteAccountDialog.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Group.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/remote/RemoteHandler.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/remote/SyncTokenStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

namespace
{
    QPointer<AccountController> g_instance;
    std::function<AccountManager*()> g_managerFactory;

    QString modeName(AccountController::Mode mode)
    {
        switch (mode) {
        case AccountController::Mode::Account:
            return QStringLiteral("account");
        case AccountController::Mode::SelfHosted:
            return QStringLiteral("selfhosted");
        case AccountController::Mode::Local:
            return QStringLiteral("local");
        default:
            return {};
        }
    }

    QString accountsRoot()
    {
        return AccountOfflineCache::accountsRoot();
    }
} // namespace

AccountController::AccountController(DatabaseTabWidget* tabs, AccountManager* account, QObject* parent)
    : QObject(parent)
    , m_tabs(tabs)
    , m_account(account ? account : (g_managerFactory ? g_managerFactory() : new AccountManager()))
    , m_sharing(new VaultSharing(m_account, this))
    , m_itemShare(new ItemShare(m_account, this))
    , m_mode(savedMode())
{
    m_account->setParent(this);
    if (!g_instance) {
        g_instance = this;
    }
    connect(m_account, &AccountManager::stateChanged, this, &AccountController::handleStateChanged);
    // SPEC §23: signed in to an account scheduled for deletion: restore it or sign out
    connect(m_account, &AccountManager::pendingDeletionChanged, this, [this] {
        if (m_account->deletionScheduledFor().isEmpty()) {
            return;
        }
        closeVaultTabs();
        PendingDeletionDialog::present(m_account, m_tabs ? m_tabs->window() : nullptr);
    });
    // Accounts a day past their erase date are gone from the server, and leave this device too
    QTimer::singleShot(0, m_account, &AccountManager::removeErasedAccounts);
    connect(m_account, &AccountManager::vaultsChanged, this, &AccountController::handleVaultsChanged);
    connect(m_account, &AccountManager::signedOut, this, [this](const QString& reason) {
        closeVaultTabs();
        if (!reason.isEmpty()) {
            emit message(reason, VaultInfoBar::Warning);
        }
        if (m_mode == Mode::Account) {
            emit startScreenRequested();
        }
    });
    // Vaults that weren't on this device during an offline unlock open once the servers answer again
    connect(m_account, &AccountManager::offlineChanged, this, [this](bool offline) {
        if (!offline && m_account->state() == AccountManager::State::Unlocked) {
            for (const auto& vault : m_account->keyring().vaults()) {
                openVault(vault.id, true);
            }
        }
    });
    connect(m_sharing, &VaultSharing::progress, this, &AccountController::busyChanged);
    if (m_tabs) {
        connect(m_tabs, &DatabaseTabWidget::databaseLocked, this, &AccountController::handleDatabaseLocked);
    }
}

AccountController::~AccountController()
{
    for (const auto& vaultId : m_vaultVersions.keys()) {
        SyncServerClient::setVaultAccount(vaultId, {});
    }
}

AccountController* AccountController::instance()
{
    return g_instance;
}

void AccountController::setAccountManagerFactory(std::function<AccountManager*()> factory)
{
    g_managerFactory = std::move(factory);
}

AccountManager* AccountController::account() const
{
    return m_account;
}

VaultSharing* AccountController::sharing() const
{
    return m_sharing;
}

ItemShare* AccountController::itemShare() const
{
    return m_itemShare;
}

AccountController::Mode AccountController::mode() const
{
    return m_mode;
}

void AccountController::setMode(Mode mode)
{
    m_mode = mode;
    if (mode == Mode::Unset) {
        config()->remove(Config::Account_StartMode);
    } else {
        config()->set(Config::Account_StartMode, modeName(mode));
    }
    emit modeChanged(mode);
}

AccountController::Mode AccountController::savedMode()
{
    const auto name = config()->get(Config::Account_StartMode).toString();
    for (auto mode : {Mode::Account, Mode::SelfHosted, Mode::Local}) {
        if (name == modeName(mode)) {
            return mode;
        }
    }
    return Mode::Unset;
}

bool AccountController::needsStartScreen() const
{
    return m_mode == Mode::Unset || (m_mode == Mode::Account && m_account->state() == AccountManager::State::SignedOut);
}

void AccountController::restore()
{
    // Other accounts are checked in the background. Without an account nothing goes out, whatever is remembered.
    if (m_mode == Mode::Account || m_mode == Mode::Unset) {
        m_account->checkSetAsideAccounts();
    }
    // Only account mode has anything to restore, and that touches the keychain, not the network
    if (m_mode != Mode::Account || m_account->state() != AccountManager::State::SignedOut) {
        return;
    }
    const auto accountId = AccountManager::lastAccountId();
    if (!accountId.isEmpty()) {
        m_account->restoreSession(accountId);
    }
}

QString AccountController::hostedServerUrl()
{
    const auto url = config()->get(Config::Account_ServerUrl).toString().trimmed();
    return url.isEmpty() ? QStringLiteral(KPXC_DEFAULT_ACCOUNT_SERVER) : url;
}

QUrl AccountController::recoveryRequestUrl(const QString& issuerUrl)
{
    auto issuer = issuerUrl.trimmed();
    if (issuer.isEmpty()) {
        issuer = hostedServerUrl();
    }
    while (issuer.endsWith('/')) {
        issuer.chop(1);
    }
    return QUrl(issuer + QStringLiteral("/recover"));
}

void AccountController::setHostedServerUrl(const QString& url)
{
    auto trimmed = url.trimmed();
    while (trimmed.endsWith('/')) {
        trimmed.chop(1);
    }
    if (trimmed.isEmpty() || trimmed == QStringLiteral(KPXC_DEFAULT_ACCOUNT_SERVER)) {
        config()->remove(Config::Account_ServerUrl);
    } else {
        config()->set(Config::Account_ServerUrl, trimmed);
    }
}

void AccountController::ensureServer(const QString& issuerUrl, Done done)
{
    auto url = issuerUrl.trimmed();
    while (url.endsWith('/')) {
        url.chop(1);
    }
    if (url.isEmpty()) {
        url = hostedServerUrl();
    }
    auto current = m_account->server().issuer;
    while (current.endsWith('/')) {
        current.chop(1);
    }
    if (m_account->server().isValid() && current == url) {
        done(true, {});
        return;
    }
    // GET <issuer>/.well-known/openvireo-configuration tells where the API and the sync server are
    auto* waiter = new QObject(this);
    auto finish = [waiter, url, done](bool ok, const QString& error) {
        if (waiter->property("done").toBool()) {
            return;
        }
        waiter->setProperty("done", true);
        waiter->deleteLater();
        if (ok) {
            setHostedServerUrl(url);
        }
        done(ok, error);
    };
    connect(m_account, &AccountManager::serverChanged, waiter, [finish] { finish(true, {}); });
    connect(m_account, &AccountManager::error, waiter, [finish](const QString& error) { finish(false, error); });
    m_account->discover(url);
}

QString AccountController::selfHostedTokenKey(const QString& url)
{
    return "ov-selfhosted-token:" + url;
}

bool AccountController::selfHostedServer(QString* url, QString* token)
{
    if (savedMode() != Mode::SelfHosted) {
        return false;
    }
    *url = config()->get(Config::Account_SelfHostedServerUrl).toString();
    *token = SyncTokenStore::load(selfHostedTokenKey(*url));
    return !url->isEmpty();
}

void AccountController::pairSelfHosted(const QString& url, const QString& code, Done done)
{
    auto serverUrl = url.trimmed();
    while (serverUrl.endsWith('/')) {
        serverUrl.chop(1);
    }
    const QUrl parsed(serverUrl);
    if (!parsed.isValid() || parsed.host().isEmpty() || (parsed.scheme() != "https" && parsed.scheme() != "http")) {
        done(false, tr("Enter a valid server URL starting with http:// or https://."));
        return;
    }
    if (code.trimmed().isEmpty()) {
        done(false, tr("Enter the pairing code from another device or from the server's admin."));
        return;
    }
    RemoteParams params;
    params.type = RemoteParams::Type::SyncServer;
    params.serverUrl = serverUrl;
    auto* client = SyncServerClient::create(params, this);
    QPointer<AccountController> guard(this);
    // GET /api/v1/health first, so a wrong URL gets a clearer message than a failed pairing
    client->health([guard, client, serverUrl, code, done](const SyncServerClient::Response& health) {
        if (!guard) {
            return;
        }
        if (health.status != 200) {
            client->deleteLater();
            done(false,
                 tr("No OpenVireo sync server answers at %1: %2")
                     .arg(serverUrl, SyncServerClient::errorMessage(health)));
            return;
        }
        client->claimPairingCode(
            code,
            RemoteHandler::deviceName(),
            [guard, client, serverUrl, done](const SyncServerClient::Response& claim) {
                client->deleteLater();
                if (!guard) {
                    return;
                }
                const auto token = QJsonDocument::fromJson(claim.body).object().value("token").toString();
                if (claim.status != 201 || token.isEmpty()) {
                    done(false, tr("Could not pair this device: %1").arg(SyncServerClient::errorMessage(claim)));
                    return;
                }
                SyncTokenStore::save(selfHostedTokenKey(serverUrl), token);
                config()->set(Config::Account_SelfHostedServerUrl, serverUrl);
                guard->setMode(Mode::SelfHosted);
                done(true, {});
            });
    });
}

QString AccountController::vaultDirectory(const QString& accountId)
{
    return accountsRoot() + "/" + accountId;
}

bool AccountController::isAccountVaultPath(const QString& filePath)
{
    if (filePath.isEmpty()) {
        return false;
    }
    const auto root = QDir::cleanPath(accountsRoot()) + "/";
    return QDir::cleanPath(QFileInfo(filePath).absoluteFilePath()).startsWith(root);
}

QString AccountController::vaultFilePath(const QString& vaultId) const
{
    return vaultDirectory(m_account->accountId()) + "/" + vaultId + ".kdbx";
}

bool AccountController::isAccountVault(const DatabaseWidget* dbWidget) const
{
    return !vaultIdOf(dbWidget).isEmpty();
}

QString AccountController::vaultIdOf(const DatabaseWidget* dbWidget) const
{
    for (auto it = m_vaultWidgets.constBegin(); it != m_vaultWidgets.constEnd(); ++it) {
        if (it.value() && it.value() == dbWidget) {
            return it.key();
        }
    }
    return {};
}

DatabaseWidget* AccountController::vaultWidget(const QString& vaultId) const
{
    return m_vaultWidgets.value(vaultId);
}

void AccountController::unlockAccount(const QString& password)
{
    m_openOnUnlock = true;
    m_account->unlock(password);
}

void AccountController::quickUnlockAccount()
{
    m_openOnUnlock = true;
    m_account->quickUnlock();
}

void AccountController::handleStateChanged()
{
    const auto state = m_account->state();
    if (state == AccountManager::State::Unlocked) {
        // Signing in also unlocks; the vaults open once they are listed
        m_openOnUnlock = true;
        if (m_mode != Mode::Account) {
            setMode(Mode::Account);
        }
    } else if (state == AccountManager::State::Locked || state == AccountManager::State::SignedOut) {
        closeVaultTabs();
    }
}

void AccountController::handleVaultsChanged()
{
    if (m_account->state() != AccountManager::State::Unlocked) {
        return;
    }
    const auto& keyring = m_account->keyring();
    for (const auto& vault : keyring.vaults()) {
        // Shared vaults name their creator's account in their settings; this device syncs them as this account
        SyncServerClient::setVaultAccount(vault.id, m_account->accountId());
    }

    // Open vaults follow key rotations, and close when this account lost access
    for (const auto& vaultId : m_vaultWidgets.keys()) {
        auto widget = m_vaultWidgets.value(vaultId);
        if (!widget) {
            m_vaultWidgets.remove(vaultId);
            continue;
        }
        if (!keyring.contains(vaultId)) {
            emit message(tr("You no longer have access to \"%1\".").arg(widget->database()->rootGroup()->name()),
                         VaultInfoBar::Warning);
            m_vaultWidgets.remove(vaultId);
            m_vaultVersions.remove(vaultId);
            m_tabs->closeDatabaseTab(widget);
            QFile::remove(vaultFilePath(vaultId));
            continue;
        }
        const auto current = keyring.vault(vaultId).currentKeyVersion;
        const auto key = keyring.compositeKey(vaultId, current);
        if (current != m_vaultVersions.value(vaultId) && key && !widget->isLocked()) {
            // The blob on the server is under the new key once a rotation is activated
            widget->database()->setKey(key, true, true);
            widget->database()->save();
            m_vaultVersions.insert(vaultId, current);
        }
    }

    if (m_openOnUnlock) {
        m_openOnUnlock = false;
        openAllVaults();
    }
}

void AccountController::openAllVaults()
{
    // The vault that was in front when the account locked comes back in front, else the first one
    const auto& keyring = m_account->keyring();
    const auto front = keyring.contains(m_lastActiveVault) || keyring.vaults().isEmpty()
                           ? m_lastActiveVault
                           : keyring.vaults().first().id;
    m_lastActiveVault.clear();
    for (const auto& vault : keyring.vaults()) {
        if (vault.id != front) {
            openVault(vault.id, true);
        }
    }
    if (!front.isEmpty()) {
        openVault(front, false);
    }
}

void AccountController::requestUnlockScreen()
{
    emit unlockScreenRequested();
}

bool AccountController::ensureAccountRemote(const QSharedPointer<Database>& db, const QString& vaultId) const
{
    RemoteSettings settings(db);
    for (const auto* params : settings.getAllRemoteParams()) {
        if (params->type == RemoteParams::Type::AccountVault && params->vaultId == vaultId) {
            return false;
        }
    }
    auto* params = new RemoteParams(m_account->vaultRemoteParams(vaultId));
    params->name = tr("OpenVireo account");
    settings.addRemoteParams(params);
    settings.saveSettings();
    return true;
}

void AccountController::openVault(const QString& vaultId, bool inBackground)
{
    if (auto widget = m_vaultWidgets.value(vaultId)) {
        if (!inBackground && m_tabs) {
            m_tabs->setCurrentWidget(widget);
        }
        return;
    }
    if (m_account->state() != AccountManager::State::Unlocked) {
        emit message(tr("Unlock your account first."), VaultInfoBar::Warning);
        return;
    }
    const auto& keyring = m_account->keyring();
    if (!keyring.contains(vaultId) || m_opening.contains(vaultId)) {
        return;
    }
    const auto path = vaultFilePath(vaultId);
    QDir().mkpath(QFileInfo(path).absolutePath());

    // The local copy first, with the vault's newest key (Account::vault_kdbx_credentials)
    if (QFile::exists(path)) {
        QString error;
        if (const auto key = m_account->vaultKey(vaultId)) {
            auto db = QSharedPointer<Database>::create();
            if (db->open(path, key, &error)) {
                addVaultTab(vaultId, db, keyring.vault(vaultId).currentKeyVersion, inBackground);
                return;
            }
        }
        if (!m_account->isOffline()) {
            rekeyLocalCopy(vaultId, inBackground);
            return;
        }
        if (!inBackground) {
            emit message(tr("The local copy of \"%1\" is under an older key of the vault. It opens once OpenVireo is "
                            "back online.")
                             .arg(keyring.vault(vaultId).name),
                         VaultInfoBar::Information);
        }
        return;
    }

    if (m_account->isOffline()) {
        // Opening every vault after an offline unlock says nothing about each; it retries once back online
        if (inBackground) {
            return;
        }
        emit message(tr("\"%1\" hasn't been downloaded to this device yet. It opens once OpenVireo is back online.")
                         .arg(keyring.vault(vaultId).name),
                     VaultInfoBar::Information);
        return;
    }
    downloadVault(vaultId, inBackground);
}

void AccountController::downloadVault(const QString& vaultId, bool inBackground)
{
    const auto path = vaultFilePath(vaultId);
    m_opening.insert(vaultId);
    emit busyChanged(tr("Downloading \"%1\"…").arg(m_account->keyring().vault(vaultId).name));
    QPointer<AccountController> guard(this);
    m_account->downloadVault(
        vaultId,
        [guard, vaultId, path, inBackground](
            QSharedPointer<Database> db, const QString& errorCode, const QString& error) {
            if (!guard) {
                return;
            }
            guard->m_opening.remove(vaultId);
            emit guard->busyChanged({});
            if (guard->m_account->state() != AccountManager::State::Unlocked) {
                return;
            }
            const auto& keyring = guard->m_account->keyring();
            const auto vault = keyring.vault(vaultId);
            if (errorCode == "not_found") {
                if (vault.role == "read") {
                    emit guard->message(tr("Nothing was uploaded to \"%1\" yet.").arg(vault.name),
                                        VaultInfoBar::Information);
                    return;
                }
                // A vault created elsewhere whose first upload never happened: this device makes it
                const auto key = keyring.vaultKey(vaultId);
                if (key.empty()) {
                    emit guard->message(tr("This account holds no key for \"%1\".").arg(vault.name),
                                        VaultInfoBar::Error);
                    return;
                }
                db = VaultKeyring::createVaultDatabase(vault.name, key);
            } else if (!db) {
                emit guard->message(tr("Could not download \"%1\": %2").arg(vault.name, error), VaultInfoBar::Error);
                return;
            }
            guard->ensureAccountRemote(db, vaultId);
            QString saveError;
            if (!db->saveAs(path, Database::Atomic, {}, &saveError)) {
                emit guard->message(tr("Could not save a local copy of \"%1\": %2").arg(vault.name, saveError),
                                    VaultInfoBar::Error);
                return;
            }
            guard->addVaultTab(vaultId, db, vault.currentKeyVersion, inBackground);
        });
}

void AccountController::rekeyLocalCopy(const QString& vaultId, bool inBackground)
{
    const auto path = vaultFilePath(vaultId);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit message(tr("Could not read the local copy of \"%1\": %2")
                         .arg(m_account->keyring().vault(vaultId).name, file.errorString()),
                     VaultInfoBar::Error);
        return;
    }
    AccountVaultSync::Request request;
    request.vaultId = vaultId;
    request.kdbx = file.readAll();
    file.close();
    request.localChanged = true;
    m_opening.insert(vaultId);
    QPointer<AccountController> guard(this);
    m_account->syncVault(request, [guard, vaultId, path, inBackground](const AccountVaultSync::Result& result) {
        if (!guard) {
            return;
        }
        guard->m_opening.remove(vaultId);
        if (guard->m_account->state() != AccountManager::State::Unlocked) {
            return;
        }
        const auto vault = guard->m_account->keyring().vault(vaultId);
        if (!result.ok()) {
            if (result.errorCode == "kdbx_error" || result.errorCode == "vault_key_mismatch"
                || result.errorCode == "decryption_failed" || result.errorCode == "no_vault_key") {
                // Only a key this account no longer holds opens it. Keep it aside instead of losing changes that
                // weren't synced, and download the vault again.
                const auto aside = path + ".old-key";
                QFile::remove(aside);
                QFile::rename(path, aside);
                emit guard->message(tr("The local copy of \"%1\" is under an older key and was kept as %2. The vault "
                                       "is downloaded again.")
                                        .arg(vault.name, QDir::toNativeSeparators(aside)),
                                    VaultInfoBar::Warning);
                guard->downloadVault(vaultId, inBackground);
                return;
            }
            emit guard->message(tr("Could not open \"%1\": %2").arg(vault.name, result.error), VaultInfoBar::Error);
            return;
        }
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(result.kdbx) != result.kdbx.size() || !out.commit()) {
            emit guard->message(tr("Could not save a local copy of \"%1\": %2").arg(vault.name, out.errorString()),
                                VaultInfoBar::Error);
            return;
        }
        auto db = QSharedPointer<Database>::create();
        QString error;
        if (!db->open(path, AccountManager::vaultCompositeKey(result.keyFile), &error)) {
            emit guard->message(tr("Could not open \"%1\": %2").arg(vault.name, error), VaultInfoBar::Error);
            return;
        }
        guard->addVaultTab(vaultId, db, vault.currentKeyVersion, inBackground);
    });
}

void AccountController::addVaultTab(const QString& vaultId,
                                    const QSharedPointer<Database>& db,
                                    int keyVersion,
                                    bool inBackground)
{
    if (ensureAccountRemote(db, vaultId)) {
        db->save();
    }
    m_vaultVersions.insert(vaultId, keyVersion);
    SyncServerClient::setVaultAccount(vaultId, m_account->accountId());
    if (!m_tabs) {
        return;
    }
    auto* widget = new DatabaseWidget(db, m_tabs);
    m_vaultWidgets.insert(vaultId, widget);
    m_tabs->addDatabaseTab(widget, inBackground);
    emit vaultOpened(vaultId, widget);
}

void AccountController::closeVaultTabs()
{
    if (m_locking) {
        return;
    }
    m_locking = true;
    if (m_tabs) {
        const auto current = vaultIdOf(m_tabs->currentDatabaseWidget());
        if (!current.isEmpty()) {
            m_lastActiveVault = current;
        }
    }
    for (const auto& vaultId : m_vaultWidgets.keys()) {
        auto widget = m_vaultWidgets.take(vaultId);
        if (!widget || !m_tabs) {
            continue;
        }
        // Keep what was changed; the next unlock syncs it
        if (!widget->isLocked() && widget->database()->isModified()) {
            widget->database()->save();
        }
        m_tabs->closeDatabaseTab(widget);
    }
    m_vaultVersions.clear();
    m_locking = false;
}

void AccountController::lockAccount()
{
    closeVaultTabs();
    m_account->lock();
}

void AccountController::handleDatabaseLocked(DatabaseWidget* dbWidget)
{
    if (m_locking || !isAccountVault(dbWidget)) {
        return;
    }
    // A vault only opens with the account's keys, so locking one locks the account. Not from inside the
    // tab widget's own locking loop.
    QTimer::singleShot(0, this, [this] {
        if (m_account->state() == AccountManager::State::Unlocked) {
            lockAccount();
        } else {
            closeVaultTabs();
        }
    });
}

void AccountController::signOut()
{
    closeVaultTabs();
    m_lastActiveVault.clear();
    m_account->signOut();
    setMode(Mode::Unset);
    emit startScreenRequested();
}

void AccountController::setAsideAccount(std::function<void()> done)
{
    closeVaultTabs();
    m_lastActiveVault.clear();
    QPointer<AccountController> guard(this);
    m_account->setAside([guard, done] {
        if (guard) {
            guard->setMode(Mode::Unset);
        }
        done();
    });
}

void AccountController::switchToAccount(const QString& accountId, std::function<void(bool ok)> done)
{
    closeVaultTabs();
    m_lastActiveVault.clear();
    QPointer<AccountController> guard(this);
    m_account->switchTo(accountId, [guard, done](bool ok) {
        if (guard) {
            guard->setMode(ok ? Mode::Account : Mode::Unset);
        }
        done(ok);
    });
}

void AccountController::acceptInvitation(const QString& linkOrToken)
{
    auto* admin = new ServiceAccountAdmin(m_account, this);
    QPointer<AccountController> guard(this);
    admin->acceptInvitation(linkOrToken, [guard, admin](bool ok, const QString& error) {
        admin->deleteLater();
        if (!guard) {
            return;
        }
        if (!ok) {
            emit guard->message(error, VaultInfoBar::Error);
            return;
        }
        emit guard->message(tr("Invitation accepted. An owner or admin of the organization has to confirm you "
                               "before you can open its vaults."),
                            VaultInfoBar::Positive);
    });
}

void AccountController::createVault(const QString& name, const QString& orgId)
{
    QPointer<AccountController> guard(this);
    m_sharing->createVault(name,
                           orgId,
                           [guard](const QString& vaultId, const QSharedPointer<Database>& db, const QString& error) {
                               if (!guard) {
                                   return;
                               }
                               emit guard->busyChanged({});
                               if (!error.isEmpty()) {
                                   emit guard->message(error, VaultInfoBar::Error);
                                   return;
                               }
                               const auto path = guard->vaultFilePath(vaultId);
                               QDir().mkpath(QFileInfo(path).absolutePath());
                               QString saveError;
                               if (!db->saveAs(path, Database::Atomic, {}, &saveError)) {
                                   emit guard->message(saveError, VaultInfoBar::Error);
                                   return;
                               }
                               guard->addVaultTab(vaultId, db, 1, false);
                           });
}

void AccountController::importAsVault(const QSharedPointer<Database>& db, const QString& name, const QString& orgId)
{
    QPointer<AccountController> guard(this);
    m_sharing->importAsVault(
        db, name, orgId, [guard](const QString& vaultId, const QSharedPointer<Database>& vaultDb, const QString& error) {
            if (!guard) {
                return;
            }
            emit guard->busyChanged({});
            if (!error.isEmpty()) {
                emit guard->message(error, VaultInfoBar::Error);
                return;
            }
            const auto path = guard->vaultFilePath(vaultId);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QString saveError;
            if (!vaultDb->saveAs(path, Database::Atomic, {}, &saveError)) {
                emit guard->message(saveError, VaultInfoBar::Error);
                return;
            }
            guard->addVaultTab(vaultId, vaultDb, 1, false);
            emit guard->message(tr("Imported as the vault \"%1\".").arg(vaultDb->rootGroup()->name()),
                                VaultInfoBar::Positive);
        });
}

void AccountController::moveToAccount(DatabaseWidget* dbWidget, const QString& name)
{
    if (!dbWidget || dbWidget->isLocked() || isAccountVault(dbWidget)) {
        emit moveFinished(false, tr("Open and unlock the database to move first."));
        return;
    }
    QPointer<DatabaseWidget> source(dbWidget);
    QPointer<AccountController> guard(this);
    const auto sourcePath = dbWidget->database()->filePath();
    // Writing the copy can touch the database's state; only the user's own unsaved changes should prompt
    const bool wasModified = dbWidget->database()->isModified();
    m_sharing->moveToAccount(dbWidget->database(),
                             name,
                             [guard, source, sourcePath, wasModified](
                                 const QString& vaultId, const QSharedPointer<Database>& db, const QString& error) {
                                 if (!guard) {
                                     return;
                                 }
                                 emit guard->busyChanged({});
                                 if (!error.isEmpty()) {
                                     emit guard->moveFinished(false, error);
                                     return;
                                 }
                                 const auto path = guard->vaultFilePath(vaultId);
                                 QDir().mkpath(QFileInfo(path).absolutePath());
                                 QString saveError;
                                 if (!db->saveAs(path, Database::Atomic, {}, &saveError)) {
                                     emit guard->moveFinished(false, saveError);
                                     return;
                                 }
                                 // The vault takes the database's place; the original file stays as it was
                                 if (source && guard->m_tabs) {
                                     if (!wasModified) {
                                         source->database()->markAsClean();
                                     }
                                     guard->m_tabs->closeDatabaseTab(source);
                                 }
                                 guard->addVaultTab(vaultId, db, 1, false);
                                 emit guard->moveFinished(true, {});
                                 emit guard->message(
                                     tr("\"%1\" is now a vault of your account. The original file %2 wasn't changed; "
                                        "you can delete it.")
                                         .arg(db->rootGroup()->name(), QDir::toNativeSeparators(sourcePath)),
                                     VaultInfoBar::Positive);
                             });
}
