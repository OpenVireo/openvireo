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

#ifndef KEEPASSXC_VAULTPANES_H
#define KEEPASSXC_VAULTPANES_H

#include <QAbstractButton>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHash>
#include <QPointer>
#include <QSharedPointer>
#include <QWidget>

#include <components/foundation/FluentElement.h>

class AccountController;
class Database;
class DatabaseTabWidget;
class DatabaseWidget;
class QAction;
class QLabel;
class QMenu;
class QTimer;

namespace fluent::basicinput
{
    class Button;
}

// Window background behind the navigation pane
class VaultCanvas : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// Content area beside the navigation pane, with a rounded top-left corner
class VaultSurface : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// Left-aligned navigation item used in the pane footer
class VaultNavButton : public QAbstractButton
{
    Q_OBJECT

public:
    VaultNavButton(const QString& glyph, const QString& text, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString m_glyph;
    bool m_hovered = false;
};

// Title of a section in the navigation pane, with a chevron; checked while the section is expanded
class VaultSectionHeader : public QAbstractButton
{
    Q_OBJECT

public:
    VaultSectionHeader(const QString& text, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

/**
 * Vault Index navigation pane: the database's group tree and tags, and links to the password generator, reports and settings.
 */
class VaultNavPane : public QWidget, public fluent::FluentElement
{
    Q_OBJECT

public:
    VaultNavPane(QWidget* groups, QWidget* parent = nullptr);

    void onThemeUpdated() override;

private:
    QLabel* m_groupsLabel;
};

/**
 * Vault Index command bar above the entry list: the current group or search,
 * its entry count, and the main entry commands.
 */
class VaultCommandBar : public QWidget, public fluent::FluentElement
{
    Q_OBJECT

public:
    explicit VaultCommandBar(QWidget* parent = nullptr);

    void setTitle(const QString& title, int entryCount);
    // Show Cancel and Save for the editor with these buttons in place of the entry commands, or the
    // entry commands again when null
    void setEditorButtons(QDialogButtonBox* buttons);
    void onThemeUpdated() override;

private:
    QAbstractButton* editorButton(std::initializer_list<QDialogButtonBox::StandardButton> which) const;

    QLabel* m_title;
    QLabel* m_count;
    QWidget* m_commands;
    QWidget* m_editCommands;
    fluent::basicinput::Button* m_cancelButton;
    fluent::basicinput::Button* m_saveButton;
    QPointer<QDialogButtonBox> m_editorButtons;
};

/**
 * Remote Sync status for the current database, shown next to the search field.
 * Hidden when the database has no Remote Sync configured; clicking it syncs, or
 * unlocks the server copy when a background sync is waiting for that.
 */
class VaultSyncChip : public QAbstractButton, public fluent::FluentElement
{
    Q_OBJECT

public:
    explicit VaultSyncChip(QWidget* parent = nullptr);

    void setDatabaseWidget(DatabaseWidget* dbWidget);
    QSize sizeHint() const override;
    void onThemeUpdated() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private slots:
    void refresh();
    void sync();

private:
    QString remoteName() const;

    QPointer<DatabaseWidget> m_dbWidget;
    QTimer* m_relativeTimeTimer;
    bool m_hovered = false;
};

/**
 * Database picker at the start of the header: shows the current database and
 * switches between the open databases in place of the tab bar. While signed in
 * to an account it also lists the account's vaults, to open or switch to.
 */
class VaultDatabasePicker : public QAbstractButton, public fluent::FluentElement
{
    Q_OBJECT

public:
    explicit VaultDatabasePicker(DatabaseTabWidget* tabs, QWidget* parent = nullptr);

    void setAccountController(AccountController* controller);
    // Vault commands listed under the account's vaults (New Vault, Share Vault, ...)
    void setAccountActions(const QList<QAction*>& actions);

    QSize sizeHint() const override;
    void onThemeUpdated() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private slots:
    void setDatabaseWidget(DatabaseWidget* dbWidget);
    void refresh();
    void showDatabaseMenu();

private:
    void addAccountVaults(QMenu* menu);

    QPointer<DatabaseTabWidget> m_tabs;
    QPointer<DatabaseWidget> m_dbWidget;
    QPointer<AccountController> m_accountController;
    QList<QPointer<QAction>> m_accountActions;
    QString m_detail;
    bool m_locked = false;
    bool m_hovered = false;
    bool m_menuOpen = false;
};

/**
 * Account button at the end of the header: shows who is signed in, and opens the
 * account commands (unlock, lock, orgs, manage, sign out). Hidden unless signed in
 * to an account.
 */
class VaultAccountButton : public QAbstractButton, public fluent::FluentElement
{
    Q_OBJECT

public:
    explicit VaultAccountButton(QWidget* parent = nullptr);

    void setAccountController(AccountController* controller);
    // The account commands in menu order; nullptr adds a separator
    void setActions(const QList<QAction*>& actions);

    QSize sizeHint() const override;
    void onThemeUpdated() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private slots:
    void refresh();
    void showAccountMenu();

private:
    QPointer<AccountController> m_controller;
    QList<QPointer<QAction>> m_actions;
    bool m_locked = false;
    bool m_hovered = false;
    bool m_menuOpen = false;
};

/**
 * Header above the database view in the Vault Index layout: the database
 * picker, search, sync status and the account button.
 */
class VaultHeaderBar : public QWidget
{
    Q_OBJECT

public:
    VaultHeaderBar(DatabaseTabWidget* tabs, QWidget* searchWidget, QWidget* parent = nullptr);

    VaultDatabasePicker* databasePicker() const;
    VaultSyncChip* syncChip() const;
    VaultAccountButton* accountButton() const;
    void setAccountController(AccountController* controller);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VaultDatabasePicker* m_databasePicker;
    VaultSyncChip* m_syncChip;
    VaultAccountButton* m_accountButton;
};

#endif // KEEPASSXC_VAULTPANES_H
