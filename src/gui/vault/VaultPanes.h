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
#include <QHash>
#include <QPointer>
#include <QSharedPointer>
#include <QWidget>

#include <components/foundation/FluentElement.h>

class Database;
class DatabaseWidget;
class QLabel;
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

/**
 * Vault Index navigation pane: the open database, its group tree and tags,
 * and links to the password generator, reports and settings.
 */
class VaultNavPane : public QWidget, public fluent::FluentElement
{
    Q_OBJECT

public:
    VaultNavPane(QWidget* groups, QWidget* parent = nullptr);

    void setDatabase(const QSharedPointer<Database>& db);
    void onThemeUpdated() override;

private slots:
    void refresh();

private:
    QSharedPointer<Database> m_db;
    QLabel* m_dbIcon;
    QLabel* m_dbName;
    QLabel* m_dbDetail;
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
    void onThemeUpdated() override;

private:
    QLabel* m_title;
    QLabel* m_count;
};

/**
 * Remote Sync status for the current database, shown next to the search field.
 * Hidden when the database has no Remote Sync configured; clicking it syncs.
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
    enum class State
    {
        Idle,
        Syncing,
        Synced,
        Failed
    };
    struct Status
    {
        State state = State::Idle;
        QDateTime lastSync;
        QString error;
    };

    QString remoteName() const;

    QPointer<DatabaseWidget> m_dbWidget;
    QHash<DatabaseWidget*, Status> m_status;
    QTimer* m_relativeTimeTimer;
    bool m_hovered = false;
};

/**
 * Header above the database view in the Vault Index layout: search and sync status.
 */
class VaultHeaderBar : public QWidget
{
    Q_OBJECT

public:
    VaultHeaderBar(QWidget* searchWidget, QWidget* parent = nullptr);

    VaultSyncChip* syncChip() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VaultSyncChip* m_syncChip;
};

#endif // KEEPASSXC_VAULTPANES_H
