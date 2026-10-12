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

#include "gui/vault/VaultWidgets.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

#include <components/menus_toolbars/private/TextEditingMenu_p.h>

namespace
{
    // Replaces the stock editing menu of text fields that aren't Fluent controls already
    class TextEditingMenuFilter : public QObject
    {
    public:
        using QObject::QObject;

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() != QEvent::ContextMenu) {
                return false;
            }
            auto* menuEvent = static_cast<QContextMenuEvent*>(event);
            QWidget* owner = nullptr;
            QMenu* menu = nullptr;

            if (auto* lineEdit = qobject_cast<QLineEdit*>(watched)) {
                // Fluent line edits show their own menu, and spin boxes add their own actions
                if (qobject_cast<fluent::textfields::LineEdit*>(lineEdit)
                    || qobject_cast<QAbstractSpinBox*>(lineEdit->parentWidget())
                    || lineEdit->contextMenuPolicy() != Qt::DefaultContextMenu) {
                    return false;
                }
                owner = lineEdit;
                menu = lineEdit->createStandardContextMenu();
            } else if (auto* viewport = qobject_cast<QWidget*>(watched)) {
                // Text edits receive context menu events on their viewport
                auto* parent = viewport->parentWidget();
                if (auto* textEdit = qobject_cast<QTextEdit*>(parent); textEdit && textEdit->viewport() == viewport) {
                    if (textEdit->contextMenuPolicy() != Qt::DefaultContextMenu) {
                        return false;
                    }
                    // The position, in document coordinates, adds link actions over a link
                    const QPoint offset(textEdit->horizontalScrollBar()->value(),
                                        textEdit->verticalScrollBar()->value());
                    owner = textEdit;
                    menu = textEdit->createStandardContextMenu(menuEvent->pos() + offset);
                } else if (auto* plainEdit = qobject_cast<QPlainTextEdit*>(parent);
                           plainEdit && plainEdit->viewport() == viewport) {
                    if (plainEdit->contextMenuPolicy() != Qt::DefaultContextMenu) {
                        return false;
                    }
                    owner = plainEdit;
                    menu = plainEdit->createStandardContextMenu();
                }
            }

            if (!owner || !menu) {
                return false;
            }
            fluent::menus_toolbars::detail::showTextEditingContextMenu(
                owner, menu, menuEvent->globalPos(), QStringLiteral("VaultTextEditingMenu"));
            event->accept();
            return true;
        }
    };
} // namespace

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

namespace
{
    constexpr int SwitchStyleMaximumWidth = 480;
    constexpr int SwitchStyleMinimumWidth = 120;
    constexpr int DescriptionGap = 2;
    constexpr int SwitchStyleTextFlags = Qt::TextWordWrap | Qt::TextHideMnemonic;
} // namespace

void VaultCheckBox::setSwitchStyle(bool switchStyle)
{
    if (m_switchStyle == switchStyle) {
        return;
    }
    m_switchStyle = switchStyle;
    setSizePolicy(switchStyle ? QSizePolicy::Expanding : QSizePolicy::Preferred, QSizePolicy::Preferred);
    updateGeometry();
    update();
}

bool VaultCheckBox::hasSwitchStyle() const
{
    return m_switchStyle;
}

void VaultCheckBox::setDescription(const QString& description)
{
    m_description = description;
    updateGeometry();
    update();
}

QString VaultCheckBox::description() const
{
    return m_description;
}

int VaultCheckBox::switchTextHeight(int width) const
{
    const QFontMetrics title(Vault::font(Typography::FontRole::Body));
    int height = title.boundingRect(QRect(0, 0, width, QWIDGETSIZE_MAX), SwitchStyleTextFlags, text()).height();
    if (!m_description.isEmpty()) {
        const QFontMetrics caption(Vault::font(Typography::FontRole::Caption));
        height +=
            DescriptionGap
            + caption.boundingRect(QRect(0, 0, width, QWIDGETSIZE_MAX), SwitchStyleTextFlags, m_description).height();
    }
    return height;
}

QSize VaultCheckBox::sizeHint() const
{
    if (!m_switchStyle) {
        return fluent::basicinput::CheckBox::sizeHint();
    }
    const QFontMetrics title(Vault::font(Typography::FontRole::Body));
    const int width = qMin(title.horizontalAdvance(VaultButton::withoutMnemonic(text())) + 1, SwitchStyleMaximumWidth);
    return {width, heightForWidth(width)};
}

QSize VaultCheckBox::minimumSizeHint() const
{
    if (!m_switchStyle) {
        return fluent::basicinput::CheckBox::minimumSizeHint();
    }
    return {SwitchStyleMinimumWidth, heightForWidth(SwitchStyleMinimumWidth)};
}

bool VaultCheckBox::hasHeightForWidth() const
{
    return m_switchStyle || fluent::basicinput::CheckBox::hasHeightForWidth();
}

int VaultCheckBox::heightForWidth(int width) const
{
    if (!m_switchStyle) {
        return fluent::basicinput::CheckBox::heightForWidth(width);
    }
    return switchTextHeight(width);
}

bool VaultCheckBox::hitButton(const QPoint& pos) const
{
    return rect().contains(pos);
}

void VaultCheckBox::paintEvent(QPaintEvent* event)
{
    if (m_switchStyle) {
        // The settings card draws the switch and the focus ring around itself
        const auto& colors = themeColors();
        QPainter painter(this);
        const QFont titleFont = Vault::font(Typography::FontRole::Body);
        const QFont captionFont = Vault::font(Typography::FontRole::Caption);
        const int textHeight = switchTextHeight(width());
        QRect area(0, (height() - textHeight) / 2, width(), textHeight);

        painter.setFont(titleFont);
        painter.setPen(isEnabled() ? colors.textPrimary : colors.textDisabled);
        QRect titleRect;
        painter.drawText(area, SwitchStyleTextFlags | Qt::AlignLeft | Qt::AlignTop, text(), &titleRect);

        if (!m_description.isEmpty()) {
            area.setTop(titleRect.bottom() + 1 + DescriptionGap);
            painter.setFont(captionFont);
            painter.setPen(isEnabled() ? colors.textSecondary : colors.textDisabled);
            painter.drawText(area, SwitchStyleTextFlags | Qt::AlignLeft | Qt::AlignTop, m_description);
        }
        return;
    }

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

VaultRadioButton::VaultRadioButton(QWidget* parent)
    : fluent::basicinput::RadioButton(parent)
{
}

VaultRadioButton::VaultRadioButton(const QString& text, QWidget* parent)
    : fluent::basicinput::RadioButton(text, parent)
{
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

VaultMenu::VaultMenu(QWidget* parent)
    : VaultMenu({}, parent)
{
}

VaultMenu::VaultMenu(const QString& title, QWidget* parent)
    : fluent::menus_toolbars::FluentMenu(title, parent)
{
}

VaultMenu* VaultMenu::addMenu(const QString& title)
{
    auto* menu = new VaultMenu(title, this);
    QMenu::addMenu(menu);
    return menu;
}

VaultMenu* VaultMenu::addMenu(const QIcon& icon, const QString& title)
{
    auto* menu = addMenu(title);
    menu->setIcon(icon);
    return menu;
}

void VaultMenu::mousePressEvent(QMouseEvent* event)
{
    m_pressedAction = actionAt(event->position().toPoint());
    FluentMenu::mousePressEvent(event);
}

void VaultMenu::mouseReleaseEvent(QMouseEvent* event)
{
    // QMenu only triggers the highlighted item on release, but on macOS the highlight can be lost between press
    // and release, such as when Fluent raises the menu on press, so a click that doesn't move would only close it.
    // Highlight the item that was pressed again so the click still goes through.
    auto* action = actionAt(event->position().toPoint());
    if (action && action == m_pressedAction && action != activeAction() && action->isEnabled()
        && !action->isSeparator() && !action->menu()) {
        setActiveAction(action);
    }
    m_pressedAction.clear();
    FluentMenu::mouseReleaseEvent(event);
}

void VaultMenu::installTextEditingMenus()
{
    qApp->installEventFilter(new TextEditingMenuFilter(qApp));
}
