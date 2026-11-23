/*
 *  Copyright (C) 2026 Bongani Mbigi
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

#include "VaultDialog.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultEditShell.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultWidgets.h"

#ifdef Q_OS_MACOS
#include "gui/osutils/macutils/MacUtils.h"
#endif

#include <components/textfields/Label.h>

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>

namespace
{
    // WinUI's content dialog padding, for the dialog when it shows as a window of its own
    constexpr int WindowPadding = 24;
    constexpr int MinimumWindowWidth = 400;

    // The bar holding the commands at the bottom of a content dialog: the window canvas below a divider
    class CommandBar : public QWidget
    {
    public:
        explicit CommandBar(QWidget* parent)
            : QWidget(parent)
        {
            Vault::followTheme(this, [this] { update(); });
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.fillRect(rect(), p.canvas);
            painter.setPen(p.divider);
            painter.drawLine(0, 0, width(), 0);
        }
    };

    bool isPrimaryRole(QDialogButtonBox::ButtonRole role)
    {
        return role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole;
    }

    QLayout* layoutHolding(QLayout* layout, QWidget* widget)
    {
        if (!layout) {
            return nullptr;
        }
        for (int i = 0; i < layout->count(); ++i) {
            auto* item = layout->itemAt(i);
            if (item->widget() == widget) {
                return layout;
            }
            if (auto* found = layoutHolding(item->layout(), widget)) {
                return found;
            }
        }
        return nullptr;
    }
} // namespace

VaultDialog::VaultDialog(QWidget* parent, Qt::WindowFlags flags)
    : QDialog(parent, flags)
{
    // Done, accepted or rejected, the dialog leaves the card, which goes with it
    connect(this, &QDialog::finished, this, [this] {
        if (m_modal) {
            dismissCard();
        }
    });
}

VaultDialog::~VaultDialog()
{
    // Destroyed while shown as a card, such as with its parent: the card has nothing left to show
    if (m_modal) {
        m_modal->deleteLater();
    }
}

void VaultDialog::setPresentation(Presentation presentation)
{
    m_presentation = presentation;
}

VaultDialog::Presentation VaultDialog::presentation() const
{
    return m_presentation;
}

void VaultDialog::setNamePrefix(const QString& prefix)
{
    m_namePrefix = prefix;
}

void VaultDialog::setMaximumDialogWidth(int width)
{
    m_maximumWidth = width;
}

void VaultDialog::setFillHeight(bool fill)
{
    m_fillHeight = fill;
}

void VaultDialog::setCommands(QDialogButtonBox* buttons)
{
    m_commandBox = buttons;
    m_commandBoxSet = true;
    m_looseCommands.clear();
}

void VaultDialog::setCommands(const QList<QAbstractButton*>& buttons)
{
    m_commandBox = nullptr;
    m_commandBoxSet = true;
    m_looseCommands.clear();
    for (auto* button : buttons) {
        m_looseCommands.append({button, nullptr, -1});
    }
}

QDialogButtonBox* VaultDialog::commandBox() const
{
    return m_commandBoxSet ? m_commandBox.data() : findCommandBox();
}

void VaultDialog::setInitialFocus(QWidget* widget)
{
    m_initialFocus = widget;
}

VaultModal* VaultDialog::modal() const
{
    return m_modal;
}

bool VaultDialog::isCard() const
{
    return m_modal;
}

bool VaultDialog::canShowCardOver(const QWidget* parent)
{
    if (!parent) {
        return false;
    }
    // Over another dialog a card would sit inside that dialog, so the dialog is a window instead
    auto* window = parent->window();
    return !qobject_cast<const QDialog*>(window) && window->isVisible() && !window->isMinimized();
}

QDialogButtonBox* VaultDialog::findCommandBox() const
{
    // The dialog's own button box, not one of a page inside it
    for (auto* box : findChildren<QDialogButtonBox*>()) {
        auto* parent = box->parentWidget();
        if (parent == this || parent == m_windowBody || qobject_cast<VaultButtonBar*>(parent)) {
            return box;
        }
    }
    return nullptr;
}

void VaultDialog::prepareToShow()
{
}

void VaultDialog::setVisible(bool visible)
{
    if (visible && !m_showingInCard && !m_modal && !isVisible()) {
        prepareToShow();
        if (m_presentation == CardOverWindow && canShowCardOver(parentWidget())) {
            presentAsCard();
            return;
        }
        applyWindowChrome();
    }
    QDialog::setVisible(visible);
}

void VaultDialog::presentAsCard()
{
    m_cardParent = parentWidget();
    m_cardWindowFlags = windowFlags();
    auto* window = parentWidget()->window();

    // The card gives the title and the buttons
    if (m_windowTitle) {
        m_windowTitle->hide();
        m_windowCommands->hide();
    }
    if (auto* own = layout()) {
        own->setContentsMargins(12, 4, 8, 4);
    }

    const auto prefix = m_namePrefix.isEmpty() ? QStringLiteral("vault") + objectName() : m_namePrefix;
    m_showingInCard = true;
    setParent(window, Qt::Widget);
    auto* modal = new VaultModal(window, this, windowTitle(), prefix);
    m_modal = modal;
    modal->setMaximumCardWidth(m_maximumWidth);
    modal->setFitToContent(!m_fillHeight);

    auto* box = commandBox();
    if (box) {
        auto* bar = new VaultButtonBar(box, nullptr, VaultButtonBar::Follow);
        modal->addFooterWidget(bar, 1);
    } else if (!m_looseCommands.isEmpty()) {
        auto* bar = new QWidget();
        bar->setObjectName("vaultButtonBar");
        auto* barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(0, 0, 0, 0);
        barLayout->setSpacing(8);
        barLayout->addStretch();
        modal->addFooterWidget(bar, 1);
        moveLooseCommands(bar, false);
    } else {
        modal->setFooterHidden(true);
    }

    connect(modal, &VaultModal::rejected, this, &QDialog::reject);
    connect(modal, &VaultModal::accepted, this, &VaultDialog::clickPrimary);

    // The first field of the form, rather than the dialog itself
    QWidget* focus = m_initialFocus;
    if (!focus) {
        for (auto* child = nextInFocusChain(); child && child != this; child = child->nextInFocusChain()) {
            if (isAncestorOf(child) && child->isVisibleTo(this) && child->isEnabled()
                && (child->focusPolicy() & Qt::TabFocus)) {
                focus = child;
                break;
            }
        }
    }
    modal->setInitialFocus(focus ? focus : this);
    modal->open();
    m_showingInCard = false;
}

void VaultDialog::dismissCard()
{
    QPointer<VaultModal> modal = m_modal;
    m_modal = nullptr;
    restoreLooseCommands();
    if (modal) {
        modal->close();
    }
    // Back to a hidden dialog of its parent, so a dialog on the stack or owned by its parent outlives the card
    m_showingInCard = true;
    setParent(m_cardParent, m_cardWindowFlags);
    m_showingInCard = false;
    if (modal) {
        modal->deleteLater();
    }
}

void VaultDialog::applyWindowChrome()
{
    if (m_windowBody) {
        m_windowTitle->show();
        m_windowCommands->setVisible(commandBox() || !m_looseCommands.isEmpty());
        if (auto* own = m_windowBody->layout()) {
            own->setContentsMargins(WindowPadding, 12, WindowPadding, WindowPadding);
        }
        return;
    }

    // The form's own layout moves into the body, between the title and the commands
    auto* box = commandBox();
    m_windowBody = new QWidget(this);
    m_windowBody->setObjectName(QStringLiteral("vaultDialogBody"));
    if (auto* own = layout()) {
        m_windowBody->setLayout(own);
        own->setContentsMargins(WindowPadding, 12, WindowPadding, WindowPadding);
    }

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    using fluent::textfields::Label;
    auto* title = new Label(this);
    title->setObjectName(QStringLiteral("vaultDialogTitle"));
    title->setFluentTypography(Typography::FontRole::Subtitle);
    title->setTextColorRole(Label::TextColorRole::Primary);
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    title->setText(windowTitle());
    title->setContentsMargins(WindowPadding, WindowPadding, WindowPadding, 0);
    outer->addWidget(title);
    m_windowTitle = title;

    outer->addWidget(m_windowBody, 1);

    m_windowCommands = new CommandBar(this);
    m_windowCommands->setObjectName(QStringLiteral("vaultDialogCommands"));
    auto* commands = new QHBoxLayout(m_windowCommands);
    commands->setContentsMargins(WindowPadding, WindowPadding, WindowPadding, WindowPadding);
    commands->setSpacing(8);
    outer->addWidget(m_windowCommands);
    if (box) {
        // Shown, so the box's buttons count as visible for Enter, but clipped away inside the bar
        auto* bar = new VaultButtonBar(box, m_windowCommands, VaultButtonBar::Adopt);
        bar->setFillWidth(true);
        box->show();
        commands->addWidget(bar, 1);
    } else if (!m_looseCommands.isEmpty()) {
        moveLooseCommands(m_windowCommands, true);
    } else {
        m_windowCommands->hide();
    }

    // The window is a content dialog's card: the layer fill, with the commands on the canvas below
    Vault::followTheme(this, [this] {
        auto pal = palette();
        pal.setColor(QPalette::Window, FluentTheme::palette().layer);
        setPalette(pal);
    });
    setAutoFillBackground(true);
}

void VaultDialog::fitWindow()
{
    // Sized once, unless the dialog sized itself; after that the window keeps the size it was given
    if (!layout() || testAttribute(Qt::WA_Resized)) {
        return;
    }
    layout()->activate();
    const int width = qBound(qMin(MinimumWindowWidth, m_maximumWidth), sizeHint().width(), m_maximumWidth);
    const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : sizeHint().height();
    resize(width, qMax(height, minimumSizeHint().height()));
}

void VaultDialog::moveLooseCommands(QWidget* host, bool fillWidth)
{
    auto* hostLayout = qobject_cast<QBoxLayout*>(host->layout());
    Q_ASSERT(hostLayout);
    for (auto& command : m_looseCommands) {
        if (!command.button) {
            continue;
        }
        auto* holder = layoutHolding(layout(), command.button);
        if (!holder && m_windowBody) {
            holder = layoutHolding(m_windowBody->layout(), command.button);
        }
        command.layout = holder;
        command.index = holder ? holder->indexOf(command.button) : -1;
        command.button->setMinimumWidth(96);
        hostLayout->addWidget(command.button, fillWidth ? 1 : 0);
        // Dialogs default the first push button in it, which would take the accent from the primary command
        if (auto* push = qobject_cast<QPushButton*>(command.button.data())) {
            push->setAutoDefault(false);
        }
    }
}

void VaultDialog::restoreLooseCommands()
{
    // Back where the form had them, so a dialog shown again finds them there
    for (auto& command : m_looseCommands) {
        if (!command.button) {
            continue;
        }
        if (auto* box = qobject_cast<QBoxLayout*>(command.layout.data())) {
            box->insertWidget(qMin(command.index, box->count()), command.button);
        } else {
            command.button->setParent(this);
        }
        command.layout = nullptr;
    }
}

void VaultDialog::clickPrimary()
{
    if (auto* box = commandBox()) {
        // The default button, else the first that accepts
        QAbstractButton* primary = nullptr;
        for (auto* button : box->buttons()) {
            if (button->isHidden() || !button->isEnabled()) {
                continue;
            }
            auto* push = qobject_cast<QPushButton*>(button);
            if (push && push->isDefault()) {
                primary = button;
                break;
            }
            if (!primary && isPrimaryRole(box->buttonRole(button))) {
                primary = button;
            }
        }
        if (primary) {
            primary->click();
        }
        return;
    }
    for (const auto& command : m_looseCommands) {
        auto* push = qobject_cast<QPushButton*>(command.button.data());
        if (push && push->isDefault() && !push->isHidden() && push->isEnabled()) {
            push->click();
            return;
        }
    }
}

void VaultDialog::keyPressEvent(QKeyEvent* event)
{
    // Enter presses the primary command, as a content dialog's does, wherever it shows
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter && (!event->modifiers() || event->modifiers() == Qt::KeypadModifier)) {
        clickPrimary();
        return;
    }
    QDialog::keyPressEvent(event);
}

void VaultDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowTitleChange) {
        if (m_modal) {
            m_modal->setTitle(windowTitle());
        }
        if (m_windowTitle) {
            m_windowTitle->setText(windowTitle());
        }
    }
    QDialog::changeEvent(event);
}

void VaultDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!isWindow() || event->spontaneous()) {
        return;
    }
    fitWindow();
    if (m_initialFocus) {
        m_initialFocus->setFocus(Qt::OtherFocusReason);
    }
    if (m_presentation == OwnWindow) {
#ifdef Q_OS_MACOS
        // A prompt for another application comes forward over it, even while OpenVireo is hidden
        if (!QApplication::activeWindow()) {
            macUtils()->raiseOwnWindow();
        }
#endif
        raise();
        activateWindow();
    }
}
