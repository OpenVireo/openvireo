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

#ifndef KEEPASSXC_ACCOUNTCONTROLLER_H
#define KEEPASSXC_ACCOUNTCONTROLLER_H

#include "gui/MessageWidget.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QUrl>

#include <functional>

class AccountManager;
class Database;
class DatabaseTabWidget;
class DatabaseWidget;
class VaultSharing;

/**
 * The desktop side of the three ways to use KeePassXC (docs/plans/accounts-and-sharing.md, "Modes"):
 *
 * - Account: the hosted KeePassXC account. Unlocking the account opens every vault it holds, each
 *   from a local copy under the app data folder (downloaded the first time), with the vault key and
 *   no password. Locking the account closes them and wipes the keys; locking any one vault locks the
 *   account.
 * - SelfHosted: the user's own sync server, paired once with a pairing code; new Remote Sync settings
 *   are pre-filled with its URL and this device's token. Databases work as they always did.
 * - Local: no account and no network at all.
 *
 * The mode chosen on the start screen is remembered, so the start screen only shows again after
 * signing out (or before the first choice).
 */
class AccountController : public QObject
{
    Q_OBJECT

public:
    enum class Mode
    {
        Unset,
        Account,
        SelfHosted,
        Local
    };
    Q_ENUM(Mode)

    using Done = std::function<void(bool ok, const QString& error)>;

    // Uses the given account manager (for tests), or makes the default one
    explicit AccountController(DatabaseTabWidget* tabs, AccountManager* account = nullptr, QObject* parent = nullptr);
    ~AccountController() override;

    // The one made by MainWindow, null without a main window
    static AccountController* instance();
    // Account managers the controller makes come from here, for tests; nullptr restores the default
    static void setAccountManagerFactory(std::function<AccountManager*()> factory);

    AccountManager* account() const;
    VaultSharing* sharing() const;

    Mode mode() const;
    void setMode(Mode mode);
    static Mode savedMode();
    // Whether the start screen should show when the app starts
    bool needsStartScreen() const;
    // After the app starts: pick the account session up from the keychain (Locked until unlocked)
    void restore();

    // The hosted account server: the built-in default (KPXC_DEFAULT_ACCOUNT_SERVER) unless changed
    static QString hostedServerUrl();
    static void setHostedServerUrl(const QString& url);
    // The account recovery request form on that server's website (SPEC §18); empty means the hosted server
    static QUrl recoveryRequestUrl(const QString& issuerUrl);
    // Ask the account server at `issuerUrl` for its configuration, unless the account already uses it
    void ensureServer(const QString& issuerUrl, Done done);

    // Self-hosted mode: the paired server and this device's token on it, for new Remote Sync settings
    static bool selfHostedServer(QString* url, QString* token);
    // Check the server's health, then trade the pairing code for this device's token
    void pairSelfHosted(const QString& url, const QString& code, Done done);

    // Local copies of account vaults live here; they never go into the recent databases
    static QString vaultDirectory(const QString& accountId);
    static bool isAccountVaultPath(const QString& filePath);
    QString vaultFilePath(const QString& vaultId) const;

    bool isAccountVault(const DatabaseWidget* dbWidget) const;
    QString vaultIdOf(const DatabaseWidget* dbWidget) const;
    DatabaseWidget* vaultWidget(const QString& vaultId) const;

public slots:
    void unlockAccount(const QString& password);
    // Unlock with Touch ID, Windows Hello or Polkit after the password unlocked the account once
    void quickUnlockAccount();
    // Save and close the open vaults, then wipe the keys
    void lockAccount();
    // Close the vaults, end the session and go back to the start screen; local databases stay open
    void signOut();
    // Open (or show) a vault in a tab, downloading it the first time
    void openVault(const QString& vaultId, bool inBackground = false);
    void openAllVaults();
    // Create an empty vault and open it: a personal one, or one of the org `orgId`
    void createVault(const QString& name, const QString& orgId = {});
    // Join an org from the invitation link or code; an owner or admin confirms the membership afterwards
    void acceptInvitation(const QString& linkOrToken);
    // Re-key a copy of the database to a fresh vault key, upload it as a new vault and open that in its place
    void moveToAccount(DatabaseWidget* dbWidget, const QString& name);
    // Make a vault of a database the import wizard made, and open it: a personal one, or one of the org `orgId`
    void importAsVault(const QSharedPointer<Database>& db, const QString& name, const QString& orgId = {});

signals:
    void modeChanged(AccountController::Mode mode);
    // Signed out, or the session ended on the server: show the start screen
    void startScreenRequested();
    void vaultOpened(const QString& vaultId, DatabaseWidget* dbWidget);
    void moveFinished(bool ok, const QString& error);
    void message(const QString& text, MessageWidget::MessageType type);
    void busyChanged(const QString& message);

private slots:
    void handleStateChanged();
    void handleVaultsChanged();
    void handleDatabaseLocked(DatabaseWidget* dbWidget);

private:
    void addVaultTab(const QString& vaultId, const QSharedPointer<Database>& db, int keyVersion, bool inBackground);
    // The vault's remote, so it syncs as an account vault whatever device created it
    bool ensureAccountRemote(const QSharedPointer<Database>& db, const QString& vaultId) const;
    void closeVaultTabs();
    static QString selfHostedTokenKey(const QString& url);

    QPointer<DatabaseTabWidget> m_tabs;
    AccountManager* m_account;
    VaultSharing* m_sharing;
    Mode m_mode = Mode::Unset;
    bool m_openOnUnlock = false;
    bool m_locking = false;
    QHash<QString, QPointer<DatabaseWidget>> m_vaultWidgets;
    // The key version each open vault is under
    QHash<QString, int> m_vaultVersions;
    QSet<QString> m_opening;
};

#endif // KEEPASSXC_ACCOUNTCONTROLLER_H
