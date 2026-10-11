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

#include "VaultSettingsModal.h"

#include "gui/CategoryListWidget.h"
#include "gui/EditWidget.h"
#include "gui/styles/FluentTheme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QTabWidget>

namespace
{
    constexpr int CardRadius = 12;
    constexpr int CardMaxWidth = 1000;
    constexpr int CardMaxHeight = 720;
    constexpr int CardMinMargin = 48;
    constexpr int NavigationWidth = 220;
    constexpr int NavigationRowHeight = 32;

    // Colors of Slack's preferences modal
    struct SlackColors
    {
        QColor backdrop;
        QColor card;
        QColor border;
        QColor text;
        QColor textSecondary;
        QColor hover;
        QColor selected;
        QColor onSelected;
        QColor primary;
        QColor primaryHover;
        QColor onPrimary;
    };

    SlackColors slackColors()
    {
        SlackColors c;
        if (FluentTheme::isDark()) {
            c.backdrop = QColor(0, 0, 0, 166);
            c.card = QColor(0x1a, 0x1d, 0x21);
            c.border = QColor(209, 210, 211, 33);
            c.text = QColor(0xd1, 0xd2, 0xd3);
            c.textSecondary = QColor(0xab, 0xab, 0xad);
            c.hover = QColor(209, 210, 211, 15);
            c.selected = QColor(0x11, 0x64, 0xa3);
        } else {
            c.backdrop = QColor(0, 0, 0, 128);
            c.card = QColor(0xff, 0xff, 0xff);
            c.border = QColor(29, 28, 29, 33);
            c.text = QColor(0x1d, 0x1c, 0x1d);
            c.textSecondary = QColor(0x61, 0x60, 0x61);
            c.hover = QColor(29, 28, 29, 10);
            c.selected = QColor(0x12, 0x64, 0xa3);
        }
        c.onSelected = Qt::white;
        c.primary = QColor(0x00, 0x7a, 0x5a);
        c.primaryHover = QColor(0x14, 0x85, 0x67);
        c.onPrimary = Qt::white;
        return c;
    }

    QFont pixelFont(const QFont& base, int pixelSize, QFont::Weight weight = QFont::Normal)
    {
        QFont font = base;
        font.setPixelSize(pixelSize);
        font.setWeight(weight);
        return font;
    }

    // Slack draws its icons in the text color, so recolor the theme icons the same way
    QPixmap tintedPixmap(const QIcon& icon, const QSize& size, qreal dpr, const QColor& color)
    {
        QPixmap pixmap = icon.pixmap(size, dpr);
        if (pixmap.isNull()) {
            return pixmap;
        }
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        return pixmap;
    }

    class ModalCard : public QWidget
    {
    public:
        using QWidget::QWidget;

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            const auto colors = slackColors();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(colors.border, 1));
            painter.setBrush(colors.card);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), CardRadius, CardRadius);
        }
    };

    class Divider : public QWidget
    {
    public:
        explicit Divider(QWidget* parent)
            : QWidget(parent)
        {
            setFixedHeight(1);
        }

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            QPainter painter(this);
            painter.fillRect(rect(), slackColors().border);
        }
    };

    class CloseButton : public QPushButton
    {
    public:
        explicit CloseButton(QWidget* parent)
            : QPushButton(parent)
        {
            setFixedSize(36, 36);
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::TabFocus);
        }

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            const auto colors = slackColors();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            if (underMouse() || isDown() || hasFocus()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(colors.hover);
                painter.drawRoundedRect(rect(), 8, 8);
            }
            painter.setPen(QPen(colors.text, 1.75, Qt::SolidLine, Qt::RoundCap));
            const QRectF cross = QRectF(rect()).adjusted(12.5, 12.5, -12.5, -12.5);
            painter.drawLine(cross.topLeft(), cross.bottomRight());
            painter.drawLine(cross.topRight(), cross.bottomLeft());
        }
    };

    // Slack's footer buttons: a green primary action and an outlined secondary one
    class ModalButton : public QPushButton
    {
    public:
        ModalButton(const QString& text, bool primary, QWidget* parent)
            : QPushButton(text, parent)
            , m_primary(primary)
        {
            setCursor(Qt::PointingHandCursor);
            setFont(pixelFont(font(), 15, QFont::Bold));
            setFixedHeight(36);
            setMinimumWidth(80);
        }

        QSize sizeHint() const override
        {
            return {qMax(80, fontMetrics().horizontalAdvance(text()) + 32), 36};
        }

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            const auto colors = slackColors();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            const bool active = underMouse() || isDown();
            if (m_primary) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(active ? colors.primaryHover : colors.primary);
            } else {
                painter.setPen(QPen(colors.border.darker(FluentTheme::isDark() ? 60 : 180), 1));
                painter.setBrush(active ? colors.hover : colors.card);
            }
            painter.drawRoundedRect(box, 8, 8);
            if (hasFocus()) {
                painter.setPen(QPen(colors.selected, 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(box.adjusted(1, 1, -1, -1), 7, 7);
            }
            painter.setPen(m_primary ? colors.onPrimary : colors.text);
            painter.drawText(rect(), Qt::AlignCenter, text());
        }

    private:
        bool m_primary;
    };

    class NavigationDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            Q_UNUSED(index)
            return {option.rect.width(), NavigationRowHeight};
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto colors = slackColors();
            const bool selected = option.state & QStyle::State_Selected;
            const bool hovered = option.state & QStyle::State_MouseOver;

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(option.rect);
            if (selected || hovered) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(selected ? colors.selected : colors.hover);
                painter->drawRoundedRect(box, 6, 6);
            }

            const QColor foreground = selected ? colors.onSelected : colors.text;
            const QSize iconSize(18, 18);
            const qreal dpr = option.widget ? option.widget->devicePixelRatioF() : 1.0;
            const auto icon = tintedPixmap(index.data(Qt::DecorationRole).value<QIcon>(), iconSize, dpr, foreground);
            const QRect iconRect(option.rect.left() + 12,
                                 option.rect.top() + (option.rect.height() - iconSize.height()) / 2,
                                 iconSize.width(),
                                 iconSize.height());
            painter->drawPixmap(iconRect, icon);

            painter->setFont(pixelFont(option.font, 15, selected ? QFont::DemiBold : QFont::Normal));
            painter->setPen(foreground);
            const QRect textRect = option.rect.adjusted(iconRect.right() + 10 - option.rect.left(), 0, -8, 0);
            const auto text = painter->fontMetrics().elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width());
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);

            if (option.state & QStyle::State_HasFocus && !selected) {
                painter->setPen(QPen(colors.selected, 2));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(box.adjusted(1, 1, -1, -1), 5, 5);
            }
            painter->restore();
        }
    };
} // namespace

VaultSettingsModal::VaultSettingsModal(QWidget* window, EditWidget* settings)
    : QWidget(window)
    , m_window(window)
    , m_settings(settings)
{
    setObjectName("vaultSettingsModal");
    setAttribute(Qt::WA_NoMousePropagation);
    setFocusPolicy(Qt::NoFocus);
    hide();

    m_card = new ModalCard(this);
    m_card->setObjectName("vaultSettingsCard");

    m_title = new QLabel(tr("Settings"), m_card);
    m_title->setObjectName("vaultSettingsTitle");
    m_title->setFont(pixelFont(font(), 22, QFont::Black));

    auto closeButton = new CloseButton(m_card);
    closeButton->setObjectName("vaultSettingsClose");
    closeButton->setAccessibleName(tr("Close"));
    closeButton->setToolTip(tr("Close"));
    connect(closeButton, &QPushButton::clicked, this, &VaultSettingsModal::reject);

    auto header = new QHBoxLayout();
    header->setContentsMargins(28, 20, 20, 16);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(closeButton);

    // The sidebar shares the settings widget's own category model and selection
    auto categories = settings->categoryList()->listWidget();
    m_navigation = new QListView(m_card);
    m_navigation->setObjectName("vaultSettingsNavigation");
    m_navigation->setModel(categories->model());
    m_navigation->setSelectionModel(categories->selectionModel());
    m_navigation->setItemDelegate(new NavigationDelegate(m_navigation));
    m_navigation->setFrameShape(QFrame::NoFrame);
    m_navigation->setFixedWidth(NavigationWidth);
    m_navigation->setSpacing(1);
    m_navigation->setMouseTracking(true);
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_navigation->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_navigation->viewport()->setAutoFillBackground(false);
    m_navigation->viewport()->setCursor(Qt::PointingHandCursor);
    m_navigation->setAccessibleName(tr("Settings categories"));

    // The modal provides the title, categories and buttons
    settings->headlineLabel()->hide();
    settings->categoryList()->hide();
    settings->buttonBox()->hide();

    auto body = new QHBoxLayout();
    body->setContentsMargins(16, 12, 20, 12);
    body->setSpacing(16);
    body->addWidget(m_navigation);
    body->addWidget(settings, 1);

    m_cancelButton = new ModalButton(tr("Cancel"), false, m_card);
    m_cancelButton->setObjectName("vaultSettingsCancel");
    connect(m_cancelButton, &QPushButton::clicked, this, &VaultSettingsModal::reject);
    m_saveButton = new ModalButton(tr("Save"), true, m_card);
    m_saveButton->setObjectName("vaultSettingsSave");
    connect(m_saveButton, &QPushButton::clicked, this, &VaultSettingsModal::accept);

    auto footer = new QHBoxLayout();
    footer->setContentsMargins(28, 16, 28, 20);
    footer->setSpacing(12);
    footer->addStretch();
    footer->addWidget(m_cancelButton);
    footer->addWidget(m_saveButton);

    auto layout = new QVBoxLayout(m_card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    m_dividers << new Divider(m_card);
    layout->addWidget(m_dividers.last());
    layout->addLayout(body, 1);
    m_dividers << new Divider(m_card);
    layout->addWidget(m_dividers.last());
    layout->addLayout(footer);

    setTabOrder(m_navigation, settings);
    setTabOrder(m_saveButton, closeButton);

    window->installEventFilter(this);
}

void VaultSettingsModal::open()
{
    if (isOpen()) {
        return;
    }
    m_previousFocus = m_window->focusWidget();

    applyColors();
    syncNavigation();
    setGeometry(m_window->rect());
    layoutCard();
    show();
    raise();
    m_navigation->setFocus(Qt::PopupFocusReason);
}

void VaultSettingsModal::close()
{
    if (!isOpen()) {
        return;
    }
    hide();
    if (m_previousFocus && m_previousFocus->isVisible()) {
        m_previousFocus->setFocus(Qt::PopupFocusReason);
    }
    m_previousFocus.clear();
}

bool VaultSettingsModal::isOpen() const
{
    return isVisible();
}

QWidget* VaultSettingsModal::card() const
{
    return m_card;
}

QListView* VaultSettingsModal::navigation() const
{
    return m_navigation;
}

void VaultSettingsModal::accept()
{
    if (auto button = m_settings->buttonBox()->button(QDialogButtonBox::Ok)) {
        button->click();
    }
}

void VaultSettingsModal::reject()
{
    if (auto button = m_settings->buttonBox()->button(QDialogButtonBox::Cancel)) {
        button->click();
    }
}

bool VaultSettingsModal::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && event->type() == QEvent::Resize && isOpen()) {
        setGeometry(m_window->rect());
        layoutCard();
    }
    return QWidget::eventFilter(watched, event);
}

void VaultSettingsModal::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto colors = slackColors();
    QPainter painter(this);
    painter.fillRect(rect(), colors.backdrop);

    // Soft shadow below the card
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QRectF card = QRectF(m_card->geometry()).translated(0, 4);
    constexpr int ShadowSteps = 8;
    for (int i = ShadowSteps; i > 0; --i) {
        painter.setBrush(QColor(0, 0, 0, 3));
        painter.drawRoundedRect(card.adjusted(-i, -i, i, i), CardRadius + i, CardRadius + i);
    }
}

void VaultSettingsModal::keyPressEvent(QKeyEvent* event)
{
    // The settings widget passes on the keys its hidden buttons would have handled
    switch (event->key()) {
    case Qt::Key_Escape:
        reject();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        accept();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

bool VaultSettingsModal::focusNextPrevChild(bool next)
{
    // Keep keyboard focus inside the modal
    QWidget* current = m_window->focusWidget();
    if (!current || !isAncestorOf(current)) {
        m_navigation->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
        return true;
    }

    QWidget* candidate = current;
    do {
        candidate = next ? candidate->nextInFocusChain() : candidate->previousInFocusChain();
        if (candidate != current && isAncestorOf(candidate) && candidate->isVisibleTo(this)
            && candidate->isEnabled() && (candidate->focusPolicy() & Qt::TabFocus)
            && !candidate->focusProxy()) {
            candidate->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            return true;
        }
    } while (candidate && candidate != current);
    return true;
}

void VaultSettingsModal::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        if (isOpen()) {
            applyColors();
        }
    }
    QWidget::changeEvent(event);
}

void VaultSettingsModal::applyColors()
{
    const auto colors = slackColors();

    QPalette titlePalette = m_title->palette();
    titlePalette.setColor(QPalette::WindowText, colors.text);
    m_title->setPalette(titlePalette);

    // The dark style sheet fills group boxes with a lighter color. Set before the palettes below, since
    // applying a style sheet polishes the widgets again and resets their palettes.
    const auto styleSheet = QStringLiteral("QGroupBox { background-color: %1; }").arg(colors.card.name());
    if (m_settings->styleSheet() != styleSheet) {
        m_settings->setStyleSheet(styleSheet);
    }

    // Let the pages sit on the card instead of the window background. The application style sheet stops
    // palettes from propagating, so set it on every page widget; only the window color is overridden, so
    // the rest still follows the theme.
    QPalette cardPalette;
    cardPalette.setColor(QPalette::Window, colors.card);
    m_settings->setPalette(cardPalette);
    for (auto widget : m_settings->findChildren<QWidget*>()) {
        if (!widget->isWindow()) {
            widget->setPalette(cardPalette);
        }
    }
    // Slack's content area is flat, so drop the boxed tab pane
    for (auto tabWidget : m_settings->findChildren<QTabWidget*>()) {
        tabWidget->setDocumentMode(true);
    }

    update();
}

void VaultSettingsModal::syncNavigation()
{
    // Pages can be hidden on the settings widget's own list, which isn't shared with the sidebar
    auto categories = m_settings->categoryList()->listWidget();
    for (int row = 0; row < categories->count(); ++row) {
        m_navigation->setRowHidden(row, categories->isRowHidden(row));
    }
    if (!m_navigation->currentIndex().isValid() && categories->count() > 0) {
        categories->setCurrentRow(0);
    }
}

void VaultSettingsModal::layoutCard()
{
    const QSize available = rect().size() - QSize(CardMinMargin * 2, CardMinMargin * 2);
    const QSize size(qBound(qMin(560, rect().width()), available.width(), CardMaxWidth),
                     qBound(qMin(420, rect().height()), available.height(), CardMaxHeight));
    QRect geometry(QPoint(), size);
    geometry.moveCenter(rect().center());
    m_card->setGeometry(geometry);
}
