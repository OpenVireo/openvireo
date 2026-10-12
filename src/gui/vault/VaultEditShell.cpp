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

#include "VaultEditShell.h"

#include "gui/CategoryListWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultPanes.h"
#include "gui/vault/VaultWidgets.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace
{
    constexpr int NavigationWidth = 220;
    constexpr int NavigationRowHeight = 36;

    // Category rows matching the main window's navigation pane: icon, label, and an accent pill when selected
    class CategoryNavDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            Q_UNUSED(index)
            // Rows span the list; the option's rect isn't laid out yet when the view asks
            const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
            return {view ? view->viewport()->width() : option.rect.width(), NavigationRowHeight};
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto p = FluentTheme::palette();
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
            const bool focused = option.state.testFlag(QStyle::State_HasFocus);

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const QRectF row = QRectF(option.rect).adjusted(4, 2, -4, -2);
            if (selected || hovered) {
                QPainterPath path;
                path.addRoundedRect(row, 4, 4);
                painter->fillPath(path, selected && hovered ? p.subtlePressed : p.subtleHover);
            }
            if (selected) {
                QPainterPath pill;
                pill.addRoundedRect(QRectF(row.left(), row.center().y() - 8, 3, 16), 1.5, 1.5);
                painter->fillPath(pill, p.accent);
            }
            if (focused && option.widget && option.widget->hasFocus()) {
                QPainterPath path;
                path.addRoundedRect(row.adjusted(1, 1, -1, -1), 4, 4);
                painter->setPen(QPen(p.text, 2));
                painter->drawPath(path);
            }

            const QRect iconRect(option.rect.left() + 16, option.rect.center().y() - 9, 18, 18);
            const auto icon = index.data(Qt::DecorationRole).value<QIcon>();
            icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal);

            painter->setFont(Vault::font(selected ? Typography::FontRole::BodyStrong : Typography::FontRole::Body));
            painter->setPen(p.text);
            const QRect textRect = option.rect.adjusted(iconRect.right() + 12 - option.rect.left(), 0, -8, 0);
            const auto text = painter->fontMetrics().elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width());
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
            painter->restore();
        }
    };

    bool isPrimaryRole(QDialogButtonBox::ButtonRole role)
    {
        return role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole;
    }

    bool isLeadingRole(QDialogButtonBox::ButtonRole role)
    {
        return role == QDialogButtonBox::ApplyRole || role == QDialogButtonBox::ResetRole
               || role == QDialogButtonBox::HelpRole || role == QDialogButtonBox::ActionRole;
    }
} // namespace

VaultButtonBar::VaultButtonBar(QDialogButtonBox* buttons, QWidget* parent)
    : QWidget(parent)
    , m_buttons(buttons)
    , m_layout(new QHBoxLayout(this))
{
    setObjectName("vaultButtonBar");
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(8);

    // The box stays shown, since DialogyWidget only clicks visible buttons for Enter and Escape, but out of
    // the layout and outside the bar's bounds, where it is clipped away
    buttons->setParent(this);
    buttons->installEventFilter(this);
    rebuild();
}

bool VaultButtonBar::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::ChildAdded:
    case QEvent::ChildRemoved:
        // Standard buttons are recreated whenever the form changes them; rebuild once they settle
        if (watched == m_buttons && !m_rebuildPending) {
            m_rebuildPending = true;
            QMetaObject::invokeMethod(this, &VaultButtonBar::rebuild, Qt::QueuedConnection);
        }
        break;
    case QEvent::EnabledChange:
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
        if (auto* source = qobject_cast<QAbstractButton*>(watched)) {
            if (auto* proxy = source->property("vaultProxy").value<QAbstractButton*>()) {
                proxy->setEnabled(source->isEnabled());
                proxy->setVisible(!source->isHidden() && !isHiddenInBar(source));
            }
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void VaultButtonBar::setButtonText(QDialogButtonBox::StandardButton button, const QString& text)
{
    m_texts.insert(button, text);
    rebuild();
}

void VaultButtonBar::setButtonHidden(QDialogButtonBox::StandardButton button, bool hidden)
{
    if (hidden) {
        m_hidden.insert(button);
    } else {
        m_hidden.remove(button);
    }
    rebuild();
}

bool VaultButtonBar::isHiddenInBar(QAbstractButton* source) const
{
    return m_buttons && m_hidden.contains(m_buttons->standardButton(source));
}

void VaultButtonBar::rebuild()
{
    m_rebuildPending = false;
    while (auto* item = m_layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!m_buttons) {
        return;
    }
    m_buttons->adjustSize();
    m_buttons->move(-m_buttons->width(), -m_buttons->height());

    QList<QAbstractButton*> leading;
    QList<QAbstractButton*> trailing;
    QList<QAbstractButton*> primary;
    for (auto* source : m_buttons->buttons()) {
        const auto role = m_buttons->buttonRole(source);
        if (isPrimaryRole(role)) {
            primary << source;
        } else if (isLeadingRole(role)) {
            leading << source;
        } else {
            trailing << source;
        }
    }

    auto addProxy = [this](QAbstractButton* source, bool accent) {
        const auto text = m_texts.value(m_buttons->standardButton(source), source->text());
        auto* proxy = new VaultButton(VaultButton::withoutMnemonic(text), this);
        proxy->setObjectName(source->objectName().isEmpty() ? QString() : "vault_" + source->objectName());
        proxy->setFluentStyle(accent ? fluent::basicinput::Button::Accent : fluent::basicinput::Button::Standard);
        proxy->setMinimumWidth(96);
        proxy->setEnabled(source->isEnabled());
        proxy->setVisible(!source->isHidden() && !isHiddenInBar(source));
        connect(proxy, &QAbstractButton::clicked, source, &QAbstractButton::click);
        source->setProperty("vaultProxy", QVariant::fromValue<QAbstractButton*>(proxy));
        source->setFocusPolicy(Qt::NoFocus);
        source->installEventFilter(this);
        m_layout->addWidget(proxy);
    };

    for (auto* source : leading) {
        addProxy(source, false);
    }
    m_layout->addStretch();
    for (auto* source : trailing) {
        addProxy(source, false);
    }
    for (auto* source : primary) {
        addProxy(source, true);
    }
}

VaultEditShell::VaultEditShell(QWidget* form, CategoryListWidget* categories)
    : QObject(form)
    , m_categories(categories)
{
}

VaultEditShell* VaultEditShell::apply(QWidget* form,
                                      CategoryListWidget* categories,
                                      QStackedWidget* pages,
                                      QDialogButtonBox* buttons,
                                      QLabel* headline)
{
    auto* shell = new VaultEditShell(form, categories);
    shell->m_pages = pages;
    shell->m_headline = headline;

    // The form shows the window canvas, like the space around the main window's panes
    Vault::paintCanvas(form);
    if (auto* layout = form->layout()) {
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
    }

    // Categories become the navigation pane
    auto* list = categories->listWidget();
    categories->setCustomItemDelegate(new CategoryNavDelegate(list), NavigationWidth);
    categories->setContentsMargins(8, 4, 8, 10);
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->setAttribute(Qt::WA_MacShowFocusRect, false);
    list->viewport()->setAttribute(Qt::WA_Hover);
    list->viewport()->setAutoFillBackground(false);
    list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; }"));

    // Headline, pages and buttons sit on the rounded content surface
    shell->m_surface = new VaultSurface();
    shell->m_surface->setObjectName("vaultEditSurface");
    pages->parentWidget()->layout()->replaceWidget(pages, shell->m_surface);

    auto* surfaceLayout = new QVBoxLayout(shell->m_surface);
    surfaceLayout->setContentsMargins(24, 18, 16, 14);
    surfaceLayout->setSpacing(12);

    shell->m_title = new QLabel(shell->m_surface);
    shell->m_title->setObjectName("vaultEditTitle");
    shell->m_title->setTextFormat(Qt::PlainText);
    shell->m_title->hide();
    surfaceLayout->addWidget(shell->m_title);
    if (headline) {
        // Leave an empty, hidden stand-in where the headline was so the form's layout keeps its shape
        auto* placeholder = new QWidget();
        placeholder->hide();
        headline->parentWidget()->layout()->replaceWidget(headline, placeholder);
        headline->setParent(shell->m_surface);
        headline->setContentsMargins(0, 0, 0, 0);
        surfaceLayout->addWidget(headline);
    }

    surfaceLayout->addWidget(pages, 1);

    // The box leaves its row in the form; the bar takes over inside the surface
    surfaceLayout->addWidget(new VaultButtonBar(buttons));

    shell->clearPageBackgrounds();
    pages->installEventFilter(shell);
    shell->onThemeUpdated();
    return shell;
}

void VaultEditShell::setTitle(const QString& title)
{
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
}

void VaultEditShell::onThemeUpdated()
{
    // Style sheets polish the widgets again and reset their palettes, so set the label colors after
    if (m_surface) {
        m_surface->setStyleSheet(Vault::formStyleSheet());
    }
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_title, Typography::FontRole::Subtitle, p.text);
    if (m_headline) {
        Vault::styleLabel(m_headline, Typography::FontRole::Subtitle, p.text);
    }
}

bool VaultEditShell::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_pages && event->type() == QEvent::ChildAdded) {
        // Pages added later, such as the reports' entry editor, also sit on the surface
        QMetaObject::invokeMethod(this, &VaultEditShell::clearPageBackgrounds, Qt::QueuedConnection);
    }
    return QObject::eventFilter(watched, event);
}

void VaultEditShell::clearPageBackgrounds()
{
    if (!m_pages) {
        return;
    }
    // Pages are wrapped in scroll areas that would otherwise paint the window color over the surface
    for (int i = 0; i < m_pages->count(); ++i) {
        auto* page = m_pages->widget(i);
        page->setAutoFillBackground(false);
        if (auto* scroll = qobject_cast<QScrollArea*>(page)) {
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->viewport()->setAutoFillBackground(false);
            if (scroll->widget()) {
                scroll->widget()->setAutoFillBackground(false);
            }
        }
    }
}
