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

#include "VaultControls.h"

#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

VaultButton::VaultButton(QWidget* parent)
    : fluent::basicinput::Button(parent)
{
}

VaultButton::VaultButton(const QString& text, QWidget* parent)
    : fluent::basicinput::Button(text, parent)
{
}

QString VaultButton::withoutMnemonic(const QString& text)
{
    QString plain;
    plain.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&') && i + 1 < text.size()) {
            ++i;
        }
        plain.append(text.at(i));
    }
    return plain;
}

bool VaultButton::event(QEvent* event)
{
    // Forms set the icon, text and default flag after construction, so catch up once they're in place
    if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
        syncAppearance();
    }
    return fluent::basicinput::Button::event(event);
}

void VaultButton::paintEvent(QPaintEvent* event)
{
    // Icons can also change later, such as a show/hide toggle
    syncAppearance();
    fluent::basicinput::Button::paintEvent(event);
}

void VaultButton::syncAppearance()
{
    // Fluent buttons draw their text as is, so show mnemonic labels plainly and keep the mnemonic as a shortcut
    if (text() != m_plainText && text().contains(QLatin1Char('&'))) {
        const auto mnemonic = QKeySequence::mnemonic(text());
        m_plainText = withoutMnemonic(text());
        setText(m_plainText);
        if (!mnemonic.isEmpty() && shortcut().isEmpty()) {
            setShortcut(mnemonic);
        }
    }

    // Fluent buttons only draw a QIcon outside the text-only layout; Fluent glyphs set their own layout
    if (!iconGlyph().isEmpty()) {
        return;
    }
    ButtonLayout layout = TextOnly;
    if (!icon().isNull()) {
        layout = text().isEmpty() ? IconOnly : IconBefore;
    }
    if (fluentLayout() != layout) {
        setFluentLayout(layout);
    }

    // The default button is the primary action, which Fluent marks with the accent color
    if (isDefault() && fluentStyle() == Standard) {
        setFluentStyle(Accent);
    }
}

VaultCheckBox::VaultCheckBox(QWidget* parent)
    : fluent::basicinput::CheckBox(parent)
{
}

VaultCheckBox::VaultCheckBox(const QString& text, QWidget* parent)
    : fluent::basicinput::CheckBox(text, parent)
{
}

bool VaultCheckBox::hitButton(const QPoint& pos) const
{
    return rect().contains(pos);
}

void VaultCheckBox::paintEvent(QPaintEvent* event)
{
    fluent::basicinput::CheckBox::paintEvent(event);
    if (!isEnabled() || checkState() != Qt::Unchecked) {
        return;
    }
    // Fluent-Qt outlines the empty box with the faint default stroke; WinUI uses the strong one
    const QRect logical(boxMargin(), (height() - boxSize()) / 2, boxSize(), boxSize());
    const QRectF box = QStyle::visualRect(layoutDirection(), rect(), logical);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(themeColors().strokeStrong, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), themeRadius().control, themeRadius().control);
}

VaultComboBox::VaultComboBox(QWidget* parent)
    : fluent::basicinput::ComboBox(parent)
{
}

void VaultComboBox::keyPressEvent(QKeyEvent* event)
{
    // Fluent-Qt drops keys unless the box has focus, to keep hover-and-scroll from changing the
    // selection. Keys can also be sent to it directly, such as typing into an editable box.
    const bool focused = hasFocus() || (lineEdit() && lineEdit()->hasFocus());
    if (!focused) {
        QComboBox::keyPressEvent(event);
        return;
    }
    fluent::basicinput::ComboBox::keyPressEvent(event);
}

void VaultComboBox::mousePressEvent(QMouseEvent* event)
{
    // Fluent-Qt opens the list on any press. As with QComboBox, a press over an editable box's
    // text starts editing instead, and only the chevron opens the list.
    if (event->button() == Qt::LeftButton && lineEdit()
        && lineEdit()->geometry().contains(event->position().toPoint())) {
        lineEdit()->setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    fluent::basicinput::ComboBox::mousePressEvent(event);
}

VaultLineEdit::VaultLineEdit(QWidget* parent)
    : fluent::textfields::LineEdit(parent)
{
    setClearButtonEnabled(false);
}
