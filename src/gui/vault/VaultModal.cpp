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

#include "VaultModal.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTabWidget>

namespace
{
    constexpr int CardRadius = 12;
    constexpr int CardMaxHeight = 720;
    constexpr int CardMinMargin = 48;

    class ModalCard : public QWidget
    {
    public:
        using QWidget::QWidget;

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            const auto colors = VaultModal::colors();
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
            painter.fillRect(rect(), VaultModal::colors().border);
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
            const auto colors = VaultModal::colors();
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
} // namespace

VaultModal::Colors VaultModal::colors()
{
    Colors c;
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
    return c;
}

QFont VaultModal::pixelFont(const QFont& base, int pixelSize, QFont::Weight weight)
{
    QFont font = base;
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

VaultModal::VaultModal(QWidget* window, QWidget* content, const QString& title, const QString& namePrefix)
    : QWidget(window)
    , m_window(window)
    , m_content(content)
    , m_initialFocus(content)
    , m_namePrefix(namePrefix)
{
    setObjectName(namePrefix + "Modal");
    setAttribute(Qt::WA_NoMousePropagation);
    setFocusPolicy(Qt::NoFocus);
    hide();

    m_card = new ModalCard(this);
    m_card->setObjectName(namePrefix + "Card");

    m_title = new QLabel(title, m_card);
    m_title->setObjectName(namePrefix + "Title");
    m_title->setFont(pixelFont(font(), 22, QFont::Black));

    m_closeButton = new CloseButton(m_card);
    m_closeButton->setObjectName(namePrefix + "Close");
    m_closeButton->setAccessibleName(tr("Close"));
    m_closeButton->setToolTip(tr("Close"));
    connect(m_closeButton, &QPushButton::clicked, this, &VaultModal::reject);

    auto header = new QHBoxLayout();
    header->setContentsMargins(28, 20, 20, 16);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(m_closeButton);

    m_body = new QHBoxLayout();
    m_body->setContentsMargins(16, 12, 20, 12);
    m_body->setSpacing(16);
    m_body->addWidget(content, 1);

    m_footer = new QHBoxLayout();
    m_footer->setContentsMargins(28, 16, 28, 20);
    m_footer->setSpacing(8);
    m_footer->addStretch();

    auto layout = new QVBoxLayout(m_card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(new Divider(m_card));
    layout->addLayout(m_body, 1);
    layout->addWidget(new Divider(m_card));
    layout->addLayout(m_footer);

    window->installEventFilter(this);
    m_card->installEventFilter(this);
}

void VaultModal::open()
{
    if (isOpen()) {
        return;
    }
    m_previousFocus = m_window->focusWidget();

    applyColors();
    prepare();
    setGeometry(m_window->rect());
    layoutCard();
    show();
    raise();
    if (m_initialFocus) {
        m_initialFocus->setFocus(Qt::PopupFocusReason);
    }
}

void VaultModal::close()
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

bool VaultModal::isOpen() const
{
    return isVisible();
}

QWidget* VaultModal::card() const
{
    return m_card;
}

QWidget* VaultModal::content() const
{
    return m_content;
}

QPushButton* VaultModal::addFooterButton(const QString& text, bool primary, const QString& name)
{
    // Styled and sized like the Close and Unlock buttons of the unlock view
    auto button = new VaultButton(text, m_card);
    button->setFluentStyle(primary ? fluent::basicinput::Button::Accent : fluent::basicinput::Button::Standard);
    button->setMinimumWidth(96);
    button->setObjectName(m_namePrefix + name);
    m_footer->addWidget(button);
    // Tab moves from the last footer button around to the close button
    setTabOrder(button, m_closeButton);
    return button;
}

void VaultModal::setInitialFocus(QWidget* widget)
{
    m_initialFocus = widget;
}

void VaultModal::setFitToContent(bool fit)
{
    m_fitToContent = fit;
}

void VaultModal::setMaximumCardWidth(int width)
{
    m_maximumCardWidth = width;
}

void VaultModal::accept()
{
    emit accepted();
}

void VaultModal::reject()
{
    emit rejected();
}

void VaultModal::prepare()
{
}

QHBoxLayout* VaultModal::bodyLayout() const
{
    return m_body;
}

bool VaultModal::eventFilter(QObject* watched, QEvent* event)
{
    const bool resized = watched == m_window && event->type() == QEvent::Resize;
    // Content that grows or shrinks, such as an expanding section, resizes a card that fits it
    const bool contentChanged = watched == m_card && event->type() == QEvent::LayoutRequest && m_fitToContent;
    if ((resized || contentChanged) && isOpen()) {
        setGeometry(m_window->rect());
        layoutCard();
    }
    return QWidget::eventFilter(watched, event);
}

void VaultModal::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), colors().backdrop);

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

void VaultModal::keyPressEvent(QKeyEvent* event)
{
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

bool VaultModal::focusNextPrevChild(bool next)
{
    // Keep keyboard focus inside the modal
    QWidget* current = m_window->focusWidget();
    if (!current || !isAncestorOf(current)) {
        if (m_initialFocus) {
            m_initialFocus->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
        }
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

void VaultModal::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        if (isOpen()) {
            applyColors();
        }
    }
    QWidget::changeEvent(event);
}

void VaultModal::applyColors()
{
    const auto colors = VaultModal::colors();

    QPalette titlePalette = m_title->palette();
    titlePalette.setColor(QPalette::WindowText, colors.text);
    m_title->setPalette(titlePalette);

    // The dark style sheet fills group boxes with a lighter color. Set before the palettes below, since
    // applying a style sheet polishes the widgets again and resets their palettes.
    const auto styleSheet = QStringLiteral("QGroupBox { background-color: %1; }").arg(colors.card.name());
    if (m_content->styleSheet() != styleSheet) {
        m_content->setStyleSheet(styleSheet);
    }

    // Let the content sit on the card instead of the window background. The application style sheet stops
    // palettes from propagating, so set it on every widget; only the window color is overridden, so the
    // rest still follows the theme.
    QPalette cardPalette;
    cardPalette.setColor(QPalette::Window, colors.card);
    m_content->setPalette(cardPalette);
    for (auto widget : m_content->findChildren<QWidget*>()) {
        if (!widget->isWindow()) {
            widget->setPalette(cardPalette);
        }
    }
    // Slack's content area is flat, so drop the boxed tab pane
    for (auto tabWidget : m_content->findChildren<QTabWidget*>()) {
        tabWidget->setDocumentMode(true);
    }

    update();
}

void VaultModal::layoutCard()
{
    const QSize available = rect().size() - QSize(CardMinMargin * 2, CardMinMargin * 2);
    const int width = qBound(qMin(560, rect().width()), available.width(), m_maximumCardWidth);
    int height = qBound(qMin(420, rect().height()), available.height(), CardMaxHeight);
    if (m_fitToContent) {
        const int needed = m_card->layout()->heightForWidth(width);
        height = qMin(height, needed > 0 ? needed : m_card->sizeHint().height());
    }
    QRect geometry(QPoint(), QSize(width, height));
    geometry.moveCenter(rect().center());
    m_card->setGeometry(geometry);
}
