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

#ifndef KEEPASSXC_VAULTWIDGETS_H
#define KEEPASSXC_VAULTWIDGETS_H

#include <QWidget>

#include <functional>

#include <components/basicinput/Button.h>
#include <design/Typography.h>

class QAction;
class QHBoxLayout;
class QLabel;

namespace Vault
{
    QFont font(Typography::FontRole role);

    // Set a label's font and text color from Fluent tokens
    void styleLabel(QLabel* label, Typography::FontRole role, const QColor& color);

    fluent::basicinput::Button* iconButton(const QString& glyph, const QString& toolTip, QWidget* parent);

    // Mirror an action on a button: clicking triggers it and the button follows its
    // enabled and visible state. Hides the button if the action does not exist.
    void bindAction(QAbstractButton* button, QAction* action);
    void bindAction(QAbstractButton* button, const QString& actionName);

    // Run apply now and again whenever the Fluent theme changes, for as long as owner exists
    void followTheme(QObject* owner, std::function<void()> apply);
    // Style sheet for forms on a Vault surface: group boxes as cards, and Fluent shapes and fills
    // for the controls that have no Fluent-Qt counterpart. Depends on the theme.
    QString formStyleSheet();
    // Fill a widget's background with the window canvas, as behind the main window's panes
    void paintCanvas(QWidget* widget);
} // namespace Vault

/**
 * One field in the entry detail card: a caption, a value and trailing action buttons.
 * The row highlights on hover so its buttons read as belonging to it.
 */
class VaultFieldRow : public QWidget
{
    Q_OBJECT

public:
    VaultFieldRow(const QString& caption, QWidget* parent = nullptr);

    QLabel* caption() const;
    QLabel* value() const;
    fluent::basicinput::Button* addButton(const QString& glyph, const QString& toolTip);
    void applyColors();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* m_caption;
    QLabel* m_value;
    QHBoxLayout* m_buttons;
    bool m_hovered = false;
};

#endif // KEEPASSXC_VAULTWIDGETS_H
