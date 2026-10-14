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

#include "AutofillService.h"

#include "AutofillHost.h"
#include "AutofillPlatform.h"
#include "AutofillProtocol.h"
#include "browser/BrowserPasskeys.h"
#include "browser/BrowserService.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryAttributes.h"
#include "core/Global.h"
#include "core/Tools.h"
#include "gui/DatabaseWidget.h"
#include "gui/MainWindow.h"
#include "gui/osutils/macutils/MacUtils.h"

#include <QJsonArray>
#include <QLocalSocket>

namespace
{
    // Coalesce the bursts of changes that edits, merges and syncs make
    constexpr int SyncDelayMs = 500;
    constexpr int MaxListedCredentials = 500;

    QByteArray fromBase64(const QJsonValue& value)
    {
        return QByteArray::fromBase64(value.toString().toLatin1());
    }

    QString toBase64(const QByteArray& bytes)
    {
        return QString::fromLatin1(bytes.toBase64());
    }

    bool passkeyFlag(const Entry* entry, const QString& key)
    {
        if (!entry->attributes()->hasKey(key)) {
            return DEFAULT_BE_FLAG;
        }
        const auto value = entry->attributes()->value(key);
        return value == "1" || value == TRUE_STR;
    }

    QJsonObject describeEntry(const Database* db, const Entry* entry)
    {
        return QJsonObject({{"recordId", AutofillCredentials::recordId(db, entry)},
                            {"title", entry->resolveMultiplePlaceholders(entry->title())},
                            {"username", entry->resolveMultiplePlaceholders(entry->username())},
                            {"url", entry->resolveUrl()},
                            {"hasPassword", !entry->password().isEmpty()},
                            {"hasPasskey", entry->hasPasskey()},
                            {"hasOneTimeCode", entry->hasValidTotp()}});
    }
} // namespace

Q_GLOBAL_STATIC(AutofillService, s_autofillService);

AutofillService::AutofillService()
    : QObject()
    , m_host(new AutofillHost(this))
{
    m_host->setPeerValidator(&AutofillPlatform::isTrustedPeer);
    connect(m_host, &AutofillHost::requestReceived, this, &AutofillService::processRequest);

    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(SyncDelayMs);
    connect(&m_syncTimer, &QTimer::timeout, this, &AutofillService::syncIdentities);

    connect(getMainWindow(), &MainWindow::databaseUnlocked, this, &AutofillService::databaseUnlocked);
    connect(getMainWindow(), &MainWindow::databaseLocked, this, &AutofillService::databaseLocked);
    connect(getMainWindow(),
            &MainWindow::databaseUnlockDialogFinished,
            this,
            &AutofillService::databaseUnlockDialogFinished);

    applyEnabled();
}

AutofillService::~AutofillService() = default;

AutofillService* AutofillService::instance()
{
    return s_autofillService;
}

bool AutofillService::isAvailable()
{
    return AutofillPlatform::isIdentityStoreAvailable();
}

bool AutofillService::isSignedForAutofill()
{
    // Cached, as the signature of the running binary doesn't change
    static const bool entitled = AutofillPlatform::hasAppGroupEntitlement();
    return entitled;
}

bool AutofillService::isEnabled() const
{
    return isAvailable() && isSignedForAutofill() && config()->get(Config::Autofill_Enabled).toBool();
}

void AutofillService::setEnabled(bool enabled)
{
    config()->set(Config::Autofill_Enabled, enabled);
    applyEnabled();
}

bool AutofillService::keepIdentitiesWhenLocked() const
{
    return config()->get(Config::Autofill_KeepIdentitiesWhenLocked).toBool();
}

void AutofillService::setKeepIdentitiesWhenLocked(bool keep)
{
    config()->set(Config::Autofill_KeepIdentitiesWhenLocked, keep);
    if (!keep) {
        // Forget what was kept for databases that are locked now
        for (auto it = m_identities.begin(); it != m_identities.end();) {
            if (!it.key() || it.key()->isLocked()) {
                it = m_identities.erase(it);
            } else {
                ++it;
            }
        }
    }
    scheduleSync();
}

QStringList AutofillService::excludedDatabases() const
{
    return config()->get(Config::Autofill_ExcludedDatabases).toStringList();
}

void AutofillService::setExcludedDatabases(const QStringList& databaseIds)
{
    config()->set(Config::Autofill_ExcludedDatabases, databaseIds);
    scheduleSync();
}

void AutofillService::applyEnabled()
{
    if (isEnabled()) {
        m_host->start(AutofillPlatform::socketPath());
        // Pick up databases that were unlocked before the provider was turned on
        for (auto* dbWidget : getMainWindow()->getOpenDatabases()) {
            if (!dbWidget->isLocked()) {
                watchDatabaseWidget(dbWidget);
            }
        }
        scheduleSync();
    } else {
        m_host->stop();
        m_syncTimer.stop();
        m_identities.clear();
        answerPendingUnlocks(AutofillProtocol::failure(AutofillProtocol::Error::Disabled));
        if (isAvailable() && isSignedForAutofill()) {
            AutofillPlatform::removeAllIdentities();
        }
    }
}

QList<QSharedPointer<Database>> AutofillService::exposedDatabases() const
{
    QList<QSharedPointer<Database>> databases;
    const auto excluded = excludedDatabases();
    for (auto* dbWidget : getMainWindow()->getOpenDatabases()) {
        if (dbWidget->isLocked() || !dbWidget->database()) {
            continue;
        }
        const auto id = Tools::uuidToHex(AutofillCredentials::databaseId(dbWidget->database().data()));
        if (!excluded.contains(id)) {
            databases << dbWidget->database();
        }
    }
    return databases;
}

bool AutofillService::hasLockedDatabases() const
{
    const auto widgets = getMainWindow()->getOpenDatabases();
    return std::any_of(widgets.begin(), widgets.end(), [](DatabaseWidget* dbWidget) { return dbWidget->isLocked(); });
}

QJsonObject AutofillService::entryNotFound() const
{
    // The entry may be in a database that is open but locked; the extension then offers to unlock it
    return AutofillProtocol::failure(hasLockedDatabases() ? AutofillProtocol::Error::Locked
                                                          : AutofillProtocol::Error::NotFound);
}

void AutofillService::watchDatabaseWidget(DatabaseWidget* dbWidget)
{
    if (!dbWidget || m_identities.contains(dbWidget)) {
        return;
    }
    m_identities.insert(dbWidget, {});
    connect(dbWidget, &DatabaseWidget::databaseModified, this, &AutofillService::scheduleSync, Qt::UniqueConnection);
    connect(dbWidget, &DatabaseWidget::databaseReplaced, this, &AutofillService::scheduleSync, Qt::UniqueConnection);
    connect(dbWidget, &QObject::destroyed, this, [this, dbWidget] {
        m_identities.remove(dbWidget);
        scheduleSync();
    });
}

void AutofillService::scheduleSync()
{
    if (isEnabled()) {
        m_syncTimer.start();
    }
}

void AutofillService::syncIdentities()
{
    if (!isEnabled()) {
        return;
    }

    const auto excluded = excludedDatabases();
    QList<AutofillIdentity> all;
    for (auto it = m_identities.begin(); it != m_identities.end(); ++it) {
        auto* dbWidget = it.key();
        if (!dbWidget->isLocked() && dbWidget->database()) {
            const auto* db = dbWidget->database().data();
            const auto id = Tools::uuidToHex(AutofillCredentials::databaseId(db));
            it.value() =
                excluded.contains(id) ? QList<AutofillIdentity>() : AutofillCredentials::identitiesForDatabase(db);
        }
        all << it.value();
    }
    AutofillPlatform::replaceIdentities(all);
}

void AutofillService::databaseUnlocked(DatabaseWidget* dbWidget)
{
    if (!isEnabled()) {
        return;
    }
    watchDatabaseWidget(dbWidget);
    scheduleSync();

    if (!m_pendingUnlocks.isEmpty()) {
        answerPendingUnlocks(AutofillProtocol::success());
        restoreWindow();
    }
}

void AutofillService::databaseLocked(DatabaseWidget* dbWidget)
{
    if (!isEnabled() || !m_identities.contains(dbWidget)) {
        return;
    }
    if (!keepIdentitiesWhenLocked()) {
        m_identities[dbWidget].clear();
    }
    scheduleSync();
}

void AutofillService::databaseUnlockDialogFinished(bool accepted, DatabaseWidget* dbWidget)
{
    Q_UNUSED(dbWidget)
    if (!accepted && !m_pendingUnlocks.isEmpty()) {
        answerPendingUnlocks(AutofillProtocol::failure(AutofillProtocol::Error::Canceled));
        restoreWindow();
    }
}

void AutofillService::answerPendingUnlocks(const QJsonObject& response)
{
    const auto pending = m_pendingUnlocks;
    m_pendingUnlocks.clear();
    for (const auto& socket : pending) {
        if (socket) {
            m_host->sendResponse(socket, response);
        }
    }
}

void AutofillService::restoreWindow()
{
    if (!m_raisedForUnlock) {
        return;
    }
    m_raisedForUnlock = false;
    // Hand the focus back to the app that asked for the credential
    if (m_wasHidden) {
        macUtils()->hideOwnWindow();
    } else {
        macUtils()->raiseLastActiveWindow();
    }
}

void AutofillService::processRequest(QLocalSocket* socket, const QJsonObject& request)
{
    const auto response = handleRequest(socket, request);
    if (!response.isEmpty()) {
        m_host->sendResponse(socket, response);
    }
}

QJsonObject AutofillService::handleRequest(QLocalSocket* socket, const QJsonObject& request)
{
    using namespace AutofillProtocol;

    if (!isEnabled()) {
        return failure(Error::Disabled);
    }

    const auto type = request.value("type").toString();
    if (type == Request::Status) {
        return success({{"unlocked", !exposedDatabases().isEmpty()}, {"locked", hasLockedDatabases()}});
    }
    if (type == Request::RequestUnlock) {
        return startUnlock(socket);
    }
    if (type == Request::ListCredentials) {
        return listCredentials(request);
    }
    if (type == Request::GetCredential) {
        return getCredential(request);
    }
    if (type == Request::GetOneTimeCode) {
        return getOneTimeCode(request);
    }
    if (type == Request::PasskeyAssert) {
        return passkeyAssert(request);
    }
    if (type == Request::PasskeyRegister) {
        return passkeyRegister(request);
    }
    return failure(Error::BadRequest);
}

QJsonObject AutofillService::startUnlock(QLocalSocket* socket)
{
    if (!exposedDatabases().isEmpty()) {
        return AutofillProtocol::success();
    }

    m_pendingUnlocks << QPointer<QLocalSocket>(socket);
    if (!m_raisedForUnlock) {
        m_raisedForUnlock = true;
        m_wasHidden = macUtils()->isHidden();
        macUtils()->raiseOwnWindow();
        getMainWindow()->bringToFront();
        emit requestUnlock();
    }
    // Answered once a database unlocks or the user cancels
    return {};
}

QJsonObject AutofillService::listCredentials(const QJsonObject& request)
{
    using namespace AutofillProtocol;

    const auto databases = exposedDatabases();
    if (databases.isEmpty()) {
        return failure(hasLockedDatabases() ? Error::Locked : Error::NotFound);
    }

    const auto kind = request.value("kind").toString();
    QList<Entry*> entries;
    if (kind == "passkey") {
        entries = AutofillCredentials::passkeyEntries(databases, request.value("rpId").toString());
    } else {
        QStringList hosts;
        for (const auto& identifier : request.value("serviceIdentifiers").toArray()) {
            hosts << identifier.toObject().value("value").toString();
        }
        entries = AutofillCredentials::matchingEntries(databases, hosts);
        if (kind == "oneTimeCode") {
            entries.erase(std::remove_if(entries.begin(), entries.end(), [](Entry* e) { return !e->hasValidTotp(); }),
                          entries.end());
        } else {
            entries.erase(
                std::remove_if(entries.begin(), entries.end(), [](Entry* e) { return e->password().isEmpty(); }),
                entries.end());
        }
    }

    std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) {
        return a->title().compare(b->title(), Qt::CaseInsensitive) < 0;
    });

    QJsonArray items;
    for (const auto* entry : entries) {
        if (items.size() >= MaxListedCredentials) {
            break;
        }
        items << describeEntry(entry->database(), entry);
    }
    return success({{"items", items}});
}

QJsonObject AutofillService::getCredential(const QJsonObject& request)
{
    auto* entry = AutofillCredentials::findEntry(exposedDatabases(), request.value("recordId").toString());
    if (!entry || entry->password().isEmpty()) {
        return entryNotFound();
    }
    return AutofillProtocol::success({{"username", entry->resolveMultiplePlaceholders(entry->username())},
                                      {"password", entry->resolveMultiplePlaceholders(entry->password())}});
}

QJsonObject AutofillService::getOneTimeCode(const QJsonObject& request)
{
    auto* entry = AutofillCredentials::findEntry(exposedDatabases(), request.value("recordId").toString());
    if (!entry || !entry->hasValidTotp()) {
        return entryNotFound();
    }
    return AutofillProtocol::success({{"code", entry->totp()}});
}

QJsonObject AutofillService::passkeyAssert(const QJsonObject& request)
{
    using namespace AutofillProtocol;

    const auto rpId = request.value("rpId").toString();
    const auto clientDataHash = fromBase64(request.value("clientDataHash"));
    if (rpId.isEmpty() || clientDataHash.size() != HASH_BYTES) {
        return failure(Error::BadRequest);
    }

    const auto databases = exposedDatabases();
    Entry* entry = nullptr;
    const auto recordId = request.value("recordId").toString();
    if (!recordId.isEmpty()) {
        entry = AutofillCredentials::findEntry(databases, recordId);
    } else {
        // Without a record the system names the credential by its id
        const auto credentialId = fromBase64(request.value("credentialId"));
        for (auto* candidate : AutofillCredentials::passkeyEntries(databases, rpId)) {
            if (AutofillCredentials::passkeyCredentialId(candidate) == credentialId) {
                entry = candidate;
                break;
            }
        }
    }
    if (!entry || !entry->hasPasskey()) {
        return entryNotFound();
    }
    // Never sign for a relying party other than the one the passkey was made for
    if (entry->attributes()->value(EntryAttributes::KPEX_PASSKEY_RELYING_PARTY) != rpId) {
        return failure(Error::Denied);
    }

    const auto assertion = browserPasskeys()->buildAssertionWithClientDataHash(
        rpId,
        clientDataHash,
        entry->attributes()->value(EntryAttributes::KPEX_PASSKEY_PRIVATE_KEY_PEM),
        passkeyFlag(entry, EntryAttributes::KPEX_PASSKEY_FLAG_BE),
        passkeyFlag(entry, EntryAttributes::KPEX_PASSKEY_FLAG_BS));
    if (assertion.signature.isEmpty()) {
        return failure(Error::Failed);
    }

    return success({{"credentialId", toBase64(AutofillCredentials::passkeyCredentialId(entry))},
                    {"userHandle", toBase64(AutofillCredentials::passkeyUserHandle(entry))},
                    {"authenticatorData", toBase64(assertion.authenticatorData)},
                    {"signature", toBase64(assertion.signature)}});
}

QJsonObject AutofillService::passkeyRegister(const QJsonObject& request)
{
    using namespace AutofillProtocol;

    const auto rpId = request.value("rpId").toString();
    const auto userName = request.value("userName").toString();
    const auto userHandle = fromBase64(request.value("userHandle"));
    if (rpId.isEmpty() || userHandle.isEmpty()) {
        return failure(Error::BadRequest);
    }

    // New passkeys go to the active database when the extension may see it, otherwise to the first one it may
    const auto databases = exposedDatabases();
    if (databases.isEmpty()) {
        return failure(hasLockedDatabases() ? Error::Locked : Error::NotFound);
    }

    // The site lists the passkeys it already has for this account, so that the user doesn't get a second one
    QList<QByteArray> excludedCredentialIds;
    for (const auto& id : request.value("excludedCredentials").toArray()) {
        excludedCredentialIds << fromBase64(id);
    }
    if (AutofillCredentials::findExcludedPasskey(databases, rpId, excludedCredentialIds)) {
        return failure(Error::Excluded);
    }

    auto db = databases.first();
    const auto active = browserService()->getDatabase();
    if (active && databases.contains(active)) {
        db = active;
    }

    QList<int> algorithms;
    for (const auto& alg : request.value("algorithms").toArray()) {
        algorithms << alg.toInt();
    }
    const auto registration = browserPasskeys()->buildRegistrationWithClientDataHash(rpId, algorithms);
    if (registration.attestationObject.isEmpty()) {
        return failure(Error::Failed);
    }

    const auto rpName = request.value("rpName").toString();
    browserService()->addPasskeyToGroup(
        db,
        nullptr,
        QStringLiteral("https://%1").arg(rpId),
        rpId,
        rpName.isEmpty() ? rpId : rpName,
        userName,
        registration.credentialId,
        QString::fromLatin1(userHandle.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)),
        QString::fromUtf8(registration.privateKeyPem));

    const auto credentialId =
        QByteArray::fromBase64(registration.credentialId.toLatin1(), QByteArray::Base64UrlEncoding);
    return success(
        {{"credentialId", toBase64(credentialId)}, {"attestationObject", toBase64(registration.attestationObject)}});
}
