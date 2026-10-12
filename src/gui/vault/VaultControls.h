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

#ifndef KEEPASSXC_VAULTCONTROLS_H
#define KEEPASSXC_VAULTCONTROLS_H

#include <QPointer>

#include <components/basicinput/Button.h>
#include <components/basicinput/CheckBox.h>
#include <components/basicinput/ComboBox.h>
#include <components/menus_toolbars/Menu.h>
#include <components/textfields/LineEdit.h>

/**
 * Fluent-Qt controls as drop-in replacements for the stock ones in .ui forms.
 *
 * The forms set icons, text and default buttons the Qt way, so these adapt
 * the Fluent controls to that instead of needing Fluent-specific setup.
 */

// Push button that lays out a QIcon set by the form and gives the default button the accent style
class VaultButton : public fluent::basicinput::Button
{
    Q_OBJECT

public:
    // Text as displayed for a Qt mnemonic label: "&Open" reads "Open" and "A && B" reads "A & B"
    static QString withoutMnemonic(const QString& text);

    explicit VaultButton(QWidget* parent = nullptr);
    explicit VaultButton(const QString& text, QWidget* parent = nullptr);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void syncAppearance();

    // Text last shown without its mnemonic, so a literal "&" left in it isn't stripped again
    QString m_plainText;
};

// Check box with WinUI's stronger outline while unchecked, so it stays visible on light surfaces
class VaultCheckBox : public fluent::basicinput::CheckBox
{
    Q_OBJECT

public:
    explicit VaultCheckBox(QWidget* parent = nullptr);
    explicit VaultCheckBox(const QString& text, QWidget* parent = nullptr);

protected:
    // The whole control toggles the box, as in WinUI, not only the box and its label
    bool hitButton(const QPoint& pos) const override;
    void paintEvent(QPaintEvent* event) override;
};

// Combo box that, like QComboBox, handles keys sent to it while it doesn't have focus
class VaultComboBox : public fluent::basicinput::ComboBox
{
    Q_OBJECT

public:
    explicit VaultComboBox(QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
};

// Line edit without Fluent's clear button unless the form enables it
class VaultLineEdit : public fluent::textfields::LineEdit
{
    Q_OBJECT

public:
    explicit VaultLineEdit(QWidget* parent = nullptr);
};

// Fluent menu for context menus, button menus and menu bar menus
class VaultMenu : public fluent::menus_toolbars::FluentMenu
{
    Q_OBJECT

public:
    explicit VaultMenu(QWidget* parent = nullptr);
    explicit VaultMenu(const QString& title, QWidget* parent = nullptr);

    // QMenu::addMenu() with a title creates a stock submenu, so create a Fluent one instead
    using QMenu::addMenu;
    VaultMenu* addMenu(const QString& title);
    VaultMenu* addMenu(const QIcon& icon, const QString& title);

    // Show Fluent editing menus instead of the stock ones for every text field in the application
    static void installTextEditingMenus();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QPointer<QAction> m_pressedAction;
};

#endif // KEEPASSXC_VAULTCONTROLS_H
