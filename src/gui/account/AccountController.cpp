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

#include "AccountController.h"

#include "config-keepassx.h"

#include "account/AccountManager.h"
#include "account/ServiceAccountAdmin.h"
#include "account/VaultKeyring.h"
#include "account/VaultSharing.h"
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
#include <QStandardPaths>
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
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/accounts";
    }
} // namespace

AccountController::AccountController(DatabaseTabWidget* tabs, AccountManager* account, QObject* parent)
    : QObject(parent)
    , m_tabs(tabs)
    , m_account(account ? account : (g_managerFactory ? g_managerFactory() : new AccountManager()))
    , m_sharing(new VaultSharing(m_account, this))
    , m_mode(savedMode())
{
    m_account->setParent(this);
    if (!g_instance) {
        g_instance = this;
    }
    connect(m_account, &AccountManager::stateChanged, this, &AccountController::handleStateChanged);
    connect(m_account, &AccountManager::vaultsChanged, this, &AccountController::handleVaultsChanged);
    connect(m_account, &AccountManager::signedOut, this, [this](const QString& reason) {
        closeVaultTabs();
        if (!reason.isEmpty()) {
            emit message(reason, MessageWidget::Warning);
        }
        if (m_mode == Mode::Account) {
            emit startScreenRequested();
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
                         MessageWidget::Warning);
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
    bool first = true;
    for (const auto& vault : m_account->keyring().vaults()) {
        openVault(vault.id, !first);
        first = false;
    }
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
        emit message(tr("Unlock your account first."), MessageWidget::Warning);
        return;
    }
    const auto& keyring = m_account->keyring();
    if (!keyring.contains(vaultId) || m_opening.contains(vaultId)) {
        return;
    }
    const auto path = vaultFilePath(vaultId);
    QDir().mkpath(QFileInfo(path).absolutePath());

    // The local copy first, with the newest key that opens it
    if (QFile::exists(path)) {
        QString error;
        for (const auto version : keyring.keyVersions(vaultId)) {
            auto db = QSharedPointer<Database>::create();
            if (db->open(path, keyring.compositeKey(vaultId, version), &error)) {
                addVaultTab(vaultId, db, version, inBackground);
                return;
            }
        }
        // Only an older key (that this account no longer holds) opens it, e.g. after a rotation while this
        // device was offline. Keep it aside instead of losing changes that weren't synced.
        const auto aside = path + ".old-key";
        QFile::remove(aside);
        QFile::rename(path, aside);
        emit message(tr("The local copy of \"%1\" is under an older key and was kept as %2. The vault is "
                        "downloaded again.")
                         .arg(keyring.vault(vaultId).name, QDir::toNativeSeparators(aside)),
                     MessageWidget::Warning);
    }

    m_opening.insert(vaultId);
    emit busyChanged(tr("Downloading \"%1\"…").arg(keyring.vault(vaultId).name));
    auto* client = SyncServerClient::create(m_account->vaultRemoteParams(vaultId), this);
    QPointer<AccountController> guard(this);
    client->download([guard, client, vaultId, path, inBackground](const SyncServerClient::Response& response) {
        client->deleteLater();
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
        if (response.status == 404) {
            if (vault.role == "read") {
                emit guard->message(tr("Nothing was uploaded to \"%1\" yet.").arg(vault.name),
                                    MessageWidget::Information);
                return;
            }
            // A vault created elsewhere whose first upload never happened: this device makes it
            auto db = VaultKeyring::createVaultDatabase(vault.name, keyring.vaultKey(vaultId, vault.currentKeyVersion));
            guard->ensureAccountRemote(db, vaultId);
            QString error;
            if (!db->saveAs(path, Database::Atomic, {}, &error)) {
                emit guard->message(error, MessageWidget::Error);
                return;
            }
            guard->addVaultTab(vaultId, db, vault.currentKeyVersion, inBackground);
            return;
        }
        if (response.status != 200) {
            emit guard->message(
                tr("Could not download \"%1\": %2").arg(vault.name, SyncServerClient::errorMessage(response)),
                MessageWidget::Error);
            return;
        }
        int version = 0;
        QString error;
        if (!keyring.openVault(vaultId, response.body, &version, &error)) {
            emit guard->message(tr("Could not open \"%1\": %2").arg(vault.name, error), MessageWidget::Error);
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(response.body) != response.body.size() || !file.commit()) {
            emit guard->message(tr("Could not save a local copy of \"%1\": %2").arg(vault.name, file.errorString()),
                                MessageWidget::Error);
            return;
        }
        auto db = QSharedPointer<Database>::create();
        if (!db->open(path, keyring.compositeKey(vaultId, version), &error)) {
            emit guard->message(error, MessageWidget::Error);
            return;
        }
        guard->addVaultTab(vaultId, db, version, inBackground);
    });
}

void AccountController::addVaultTab(const QString& vaultId,
                                    const QSharedPointer<Database>& db,
                                    int keyVersion,
                                    bool inBackground)
{
    const auto& keyring = m_account->keyring();
    bool changed = ensureAccountRemote(db, vaultId);
    const auto current = keyring.vault(vaultId).currentKeyVersion;
    if (keyVersion != current && keyring.compositeKey(vaultId, current)) {
        // Opened with the key from before a rotation; the server's copy is under the new one
        db->setKey(keyring.compositeKey(vaultId, current), true, true);
        keyVersion = current;
        changed = true;
    }
    if (changed) {
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
    m_account->signOut();
    setMode(Mode::Unset);
    emit startScreenRequested();
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
            emit guard->message(error, MessageWidget::Error);
            return;
        }
        emit guard->message(tr("Invitation accepted. An owner or admin of the organization has to confirm you "
                               "before you can open its vaults."),
                            MessageWidget::Positive);
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
                                   emit guard->message(error, MessageWidget::Error);
                                   return;
                               }
                               const auto path = guard->vaultFilePath(vaultId);
                               QDir().mkpath(QFileInfo(path).absolutePath());
                               QString saveError;
                               if (!db->saveAs(path, Database::Atomic, {}, &saveError)) {
                                   emit guard->message(saveError, MessageWidget::Error);
                                   return;
                               }
                               guard->addVaultTab(vaultId, db, 1, false);
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
                                     MessageWidget::Positive);
                             });
}
