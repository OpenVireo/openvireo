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

#include "gui/styles/FluentTheme.h"
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

const QMargins VaultLineEdit::LargePadding(12, 10, 12, 10);

void VaultLineEdit::showEvent(QShowEvent* event)
{
    // Checked on show rather than once, since some forms recreate their fields after being set up
    for (auto ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (ancestor->property(LargePaddingProperty).toBool()) {
            setContentMargins(LargePadding);
            break;
        }
    }
    fluent::textfields::LineEdit::showEvent(event);
}

namespace
{
    const QMargins FieldContentMargins(::Spacing::Padding::TextFieldHorizontal,
                                       ::Spacing::Padding::TextFieldVertical,
                                       ::Spacing::Padding::TextFieldHorizontal,
                                       ::Spacing::Padding::TextFieldVertical);

    // Style sheet padding for a framed field, inside the 1px the frame's border takes
    QString fieldPadding(const QMargins& margins)
    {
        return QStringLiteral("padding: %1px %2px %3px %4px;")
            .arg(qMax(0, margins.top() - 1))
            .arg(qMax(0, margins.right() - 1))
            .arg(qMax(0, margins.bottom() - 1))
            .arg(qMax(0, margins.left() - 1));
    }
} // namespace

void VaultFieldFrame::install(QWidget* field)
{
    if (!field->findChild<VaultFieldFrame*>(QString(), Qt::FindDirectChildrenOnly)) {
        new VaultFieldFrame(field);
    }
}

VaultFieldFrame::VaultFieldFrame(QWidget* field)
    : QObject(field)
    , m_field(field)
{
    field->setAttribute(Qt::WA_Hover);
    if (auto* area = qobject_cast<QAbstractScrollArea*>(field)) {
        area->setFrameShape(QFrame::NoFrame);
        area->viewport()->setAutoFillBackground(false);
    }
    field->installEventFilter(this);
    applyPalette();
}

void VaultFieldFrame::onThemeUpdated()
{
    applyPalette();
    m_field->update();
}

void VaultFieldFrame::applyPalette()
{
    const auto& c = themeColorsRef();
    QPalette pal = m_field->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Window, Qt::transparent);
    pal.setColor(QPalette::Text, c.textPrimary);
    pal.setColor(QPalette::PlaceholderText, c.textTertiary);
    pal.setColor(QPalette::Highlight, c.accentDefault);
    pal.setColor(QPalette::HighlightedText, c.textOnAccent);
    pal.setColor(QPalette::Inactive, QPalette::Highlight, c.accentDefault);
    pal.setColor(QPalette::Inactive, QPalette::HighlightedText, c.textOnAccent);
    pal.setColor(QPalette::Disabled, QPalette::Text, c.textDisabled);
    if (pal != m_field->palette()) {
        m_field->setPalette(pal);
    }
}

bool VaultFieldFrame::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_field) {
        switch (event->type()) {
        case QEvent::Paint:
            // Runs before the field paints, so its text lands on the frame
            paint();
            break;
        case QEvent::StyleChange:
            // A style sheet repolishes the field and can bring back the stock colors
            applyPalette();
            break;
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
        case QEvent::EnabledChange:
        case QEvent::ReadOnlyChange:
            m_field->update();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

// Same fill, border and focus underline as fluent::textfields::LineEdit
void VaultFieldFrame::paint()
{
    const auto& colors = themeColorsRef();
    const bool readOnly = m_field->property("readOnly").toBool();
    QColor fill;
    QColor border;
    QColor underline;
    int underlineWidth = ::Spacing::Border::Normal;
    if (!m_field->isEnabled()) {
        fill = colors.controlDisabled;
        border = colors.strokeDivider;
        underline = border;
    } else if (readOnly) {
        fill = colors.controlAltSecondary;
        border = colors.strokeDefault;
        underline = colors.strokeDivider;
    } else if (m_field->hasFocus()) {
        fill = effectiveThemeUsesDarkAppearance() ? colors.bgSolid : colors.controlDefault;
        border = colors.strokeSecondary;
        underline = colors.accentDefault;
        underlineWidth = ::Spacing::Border::Focused;
    } else if (m_field->underMouse()) {
        fill = colors.controlSecondary;
        border = colors.strokeSecondary;
        underline = colors.strokeSecondary;
    } else {
        fill = colors.controlDefault;
        border = colors.strokeDefault;
        underline = colors.strokeDivider;
    }

    QPainter painter(m_field);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal radius = themeRadius().control;
    const QRectF frame = QRectF(m_field->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(frame, radius, radius);

    if (m_field->isEnabled() && !readOnly) {
        QPen pen(underline, underlineWidth);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const qreal y = m_field->rect().bottom() + 1 - underlineWidth / 2.0;
        painter.drawLine(QPointF(frame.left() + radius, y), QPointF(frame.right() - radius, y));
    }
}

VaultPlainTextEdit::VaultPlainTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setContentMargins(FieldContentMargins);
    VaultFieldFrame::install(this);
}

QMargins VaultPlainTextEdit::contentMargins() const
{
    return m_contentMargins;
}

void VaultPlainTextEdit::setContentMargins(const QMargins& margins)
{
    m_contentMargins = margins;
    // The transparent border keeps the text and scroll bar inside the painted frame. This sheet also wins over
    // the forms' own QPlainTextEdit rules, which would draw a stock frame over the Fluent one.
    setStyleSheet(QStringLiteral("QPlainTextEdit { background: transparent; border: 1px solid transparent; %1 }")
                      .arg(fieldPadding(margins)));
}

VaultDateTimeEdit::VaultDateTimeEdit(QWidget* parent)
    : QDateTimeEdit(parent)
    , m_contentMargins(FieldContentMargins)
{
    applyStyleSheet();
    VaultFieldFrame::install(this);
}

QMargins VaultDateTimeEdit::contentMargins() const
{
    return m_contentMargins;
}

void VaultDateTimeEdit::setContentMargins(const QMargins& margins)
{
    m_contentMargins = margins;
    applyStyleSheet();
}

void VaultDateTimeEdit::applyStyleSheet()
{
    // The drop-down keeps its room and click target, but its arrow is painted as a Fluent chevron instead
    setStyleSheet(QStringLiteral("QDateTimeEdit { background: transparent; border: 1px solid transparent; %1 }"
                                 "QDateTimeEdit::drop-down { subcontrol-origin: border; subcontrol-position: center right;"
                                 " width: 32px; border: none; background: transparent; }"
                                 "QDateTimeEdit::down-arrow { image: none; width: 0; height: 0; }")
                      .arg(fieldPadding(m_contentMargins)));
}

void VaultDateTimeEdit::paintEvent(QPaintEvent* event)
{
    QDateTimeEdit::paintEvent(event);
    if (!calendarPopup()) {
        return;
    }

    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont iconFont(Typography::FontFamily::FluentIcons);
    iconFont.setPixelSize(Typography::IconSize::Compact);
    painter.setFont(iconFont);
    painter.setPen(isEnabled() && !isReadOnly() ? p.textSecondary : p.textDisabled);
    const QRect logical(width() - 32, 0, 32, height());
    painter.drawText(QStyle::visualRect(layoutDirection(), rect(), logical),
                     Qt::AlignCenter,
                     Typography::Icons::ChevronDownMed);
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
