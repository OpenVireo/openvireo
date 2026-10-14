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

#ifndef KEEPASSXC_AUTOFILLSERVICE_H
#define KEEPASSXC_AUTOFILLSERVICE_H

#include "AutofillCredentials.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QTimer>

class AutofillHost;
class Database;
class DatabaseWidget;
class QLocalSocket;

/**
 * Makes KeePassXC a macOS AutoFill credential provider.
 *
 * The provider is a sandboxed app extension that cannot read the databases, so
 * this service keeps the system's credential identity store in step with the
 * unlocked databases and answers the extension's requests for secrets over a
 * socket in the App Group container. See docs/plans/macos-autofill-provider.md.
 */
class AutofillService : public QObject
{
    Q_OBJECT

public:
    explicit AutofillService();
    ~AutofillService() override;
    static AutofillService* instance();

    // Whether this macOS version can run the provider at all
    static bool isAvailable();
    // Whether this build is signed with the App Group the extension shares. Without it, merely looking for the
    // group container makes macOS ask the user for access to other apps' data, so the service stays off.
    static bool isSignedForAutofill();

    bool isEnabled() const;
    void setEnabled(bool enabled);
    bool keepIdentitiesWhenLocked() const;
    void setKeepIdentitiesWhenLocked(bool keep);
    QStringList excludedDatabases() const;
    void setExcludedDatabases(const QStringList& databaseIds);

    // Unlocked databases whose entries the extension may see
    QList<QSharedPointer<Database>> exposedDatabases() const;

    // Answer one request from the extension; requestUnlock may answer later through the host
    QJsonObject handleRequest(QLocalSocket* socket, const QJsonObject& request);

signals:
    void requestUnlock();

public slots:
    void scheduleSync();

private slots:
    void databaseUnlocked(DatabaseWidget* dbWidget);
    void databaseLocked(DatabaseWidget* dbWidget);
    void databaseUnlockDialogFinished(bool accepted, DatabaseWidget* dbWidget);
    void processRequest(QLocalSocket* socket, const QJsonObject& request);
    void syncIdentities();

private:
    void applyEnabled();
    void watchDatabaseWidget(DatabaseWidget* dbWidget);
    bool hasLockedDatabases() const;
    QJsonObject entryNotFound() const;
    void answerPendingUnlocks(const QJsonObject& response);
    void restoreWindow();

    QJsonObject listCredentials(const QJsonObject& request);
    QJsonObject getCredential(const QJsonObject& request);
    QJsonObject getOneTimeCode(const QJsonObject& request);
    QJsonObject passkeyAssert(const QJsonObject& request);
    QJsonObject passkeyRegister(const QJsonObject& request);
    QJsonObject passkeyCheckExcluded(const QJsonObject& request);
    QJsonObject startUnlock(QLocalSocket* socket);

    QPointer<AutofillHost> m_host;
    QTimer m_syncTimer;
    // Identities of each open database, kept for locked ones when the user asks for that
    QHash<DatabaseWidget*, QList<AutofillIdentity>> m_identities;
    QList<QPointer<QLocalSocket>> m_pendingUnlocks;
    bool m_raisedForUnlock = false;
    bool m_wasHidden = false;

    Q_DISABLE_COPY(AutofillService)

    friend class TestAutofill;
};

static inline AutofillService* autofillService()
{
    return AutofillService::instance();
}

#endif // KEEPASSXC_AUTOFILLSERVICE_H
