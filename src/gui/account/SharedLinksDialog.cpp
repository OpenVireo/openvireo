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

#include "SharedLinksDialog.h"

#include "ShareLinkDialog.h"
#include "core/Clock.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultInfoBar.h"
#include "gui/vault/VaultMetrics.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultWidgets.h"

#include <components/navigation/SelectorBar.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QVBoxLayout>

namespace
{
    // The card's width over the main window, as in the mockup
    constexpr int CardWidth = 640;
    // WinUI's content dialog padding, for the dialog when it opens as a window of its own
    constexpr int WindowPadding = 24;
    // Room for about four links before the list scrolls
    constexpr int ListHeight = 312;
    constexpr int TileSize = 36;

    QString opensText(const ItemShare::Listing& share)
    {
        return share.maxOpens > 0 ? SharedLinksDialog::tr("opened %1 of %2").arg(share.opens).arg(share.maxOpens)
                                  : SharedLinksDialog::tr("%n open(s)", "", share.opens);
    }

    VaultButton* pillButton(const QString& text, QWidget* parent, bool accent = false)
    {
        auto* button = new VaultButton(text, parent);
        if (accent) {
            button->setFluentStyle(fluent::basicinput::Button::Accent);
        }
        Vault::makePill(button);
        return button;
    }
} // namespace

QString SharedLinksDialog::endReasonText(const QString& reason)
{
    if (reason == "opened") {
        return tr("Opened");
    }
    if (reason == "revoked") {
        return tr("Revoked");
    }
    if (reason == "passcode") {
        return tr("Too many wrong passcodes");
    }
    if (reason == "access_lost") {
        return tr("You no longer have the vault");
    }
    if (reason == "policy") {
        return tr("Turned off by your organization");
    }
    return tr("Expired");
}

SharedLinksDialog::SharedLinksDialog(ItemShare* shares, QWidget* parent)
    : QDialog(parent)
    , m_shares(shares)
    , m_message(new VaultInfoBar(this))
    , m_intro(new QLabel(this))
    , m_tabs(new fluent::navigation::SelectorBar(this))
    , m_buttonRow(new QWidget(this))
{
    setObjectName("sharedLinksDialog");
    setWindowTitle(tr("Shared Links"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(WindowPadding, WindowPadding, WindowPadding, WindowPadding);
    layout->setSpacing(14);

    m_intro->setText(tr("Links open a copy of an entry or some text in any browser, without an account."));
    m_intro->setWordWrap(true);
    layout->addWidget(m_intro);

    m_tabs->setObjectName("sharedLinksTabs");
    m_tabs->addItem(tr("Active"));
    m_tabs->addItem(tr("Ended"));
    m_tabs->setSelectedIndex(0);
    m_tabs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(m_tabs, &fluent::navigation::SelectorBar::selectedIndexChanged, this, [this](int index) {
        m_showEnded = index == 1;
        m_confirming.clear();
        fill();
    });
    layout->addWidget(m_tabs, 0, Qt::AlignLeft);

    m_message->setObjectName("messageWidget");
    m_message->setHidden(true);
    layout->addWidget(m_message);

    // The links, on a panel that scrolls, or what to do when there are none
    m_listPanel = new VaultPanel(VaultPanel::Layer, this);
    m_listPanel->setObjectName("sharedLinksList");
    m_listPanel->setFixedHeight(ListHeight);
    auto* panelLayout = new QVBoxLayout(m_listPanel);
    panelLayout->setContentsMargins(2, 2, 2, 2);
    panelLayout->setSpacing(0);

    auto* scroll = new QScrollArea(m_listPanel);
    m_scroll = scroll;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Transparent, so the panel's rounded fill and border show around the rows
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget "
                                         "{ background: transparent; }"));
    scroll->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    m_rows = new QWidget(scroll);
    m_rows->setObjectName("sharedLinksRows");
    m_rows->setAutoFillBackground(false);
    m_rowsLayout = new QVBoxLayout(m_rows);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);
    m_rowsLayout->addStretch();
    scroll->setWidget(m_rows);
    panelLayout->addWidget(scroll);

    m_empty = new QWidget(m_listPanel);
    m_empty->setObjectName("sharedLinksEmpty");
    auto* emptyLayout = new QVBoxLayout(m_empty);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->setSpacing(8);
    emptyLayout->addStretch();
    m_emptyTitle = new QLabel(m_empty);
    m_emptyTitle->setAlignment(Qt::AlignHCenter);
    m_emptyText = new QLabel(m_empty);
    m_emptyText->setAlignment(Qt::AlignHCenter);
    m_emptyText->setWordWrap(true);
    m_emptyAction = pillButton(QString(), m_empty);
    m_emptyAction->setObjectName("sharedLinksSeeEnded");
    connect(m_emptyAction, &QPushButton::clicked, this, [this] { m_tabs->setSelectedIndex(1); });
    emptyLayout->addWidget(m_emptyTitle);
    emptyLayout->addWidget(m_emptyText);
    emptyLayout->addSpacing(4);
    emptyLayout->addWidget(m_emptyAction, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    panelLayout->addWidget(m_empty);
    layout->addWidget(m_listPanel);

    // The footer: how to share an entry, Share Text…, and Close when it's a window
    auto* buttons = new QHBoxLayout(m_buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    m_hint = new QLabel(tr("To share an entry, use its context menu."), m_buttonRow);
    m_hint->setWordWrap(true);
    buttons->addWidget(m_hint, 1);
    m_closeButton = pillButton(tr("Close"), m_buttonRow);
    m_closeButton->setObjectName("sharedLinksClose");
    m_shareText = pillButton(tr("Share Text…"), m_buttonRow, true);
    m_shareText->setObjectName("sharedLinksShareText");
    m_shareText->setIconGlyph(Typography::Icons::Add);
    buttons->addWidget(m_closeButton);
    buttons->addWidget(m_shareText);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_shareText, &QPushButton::clicked, this, &SharedLinksDialog::shareText);
    layout->addWidget(m_buttonRow);

    Vault::followTheme(this, [this] {
        applyColors();
        fill();
    });
    setFixedWidth(CardWidth);
    reload();
}

SharedLinksDialog* SharedLinksDialog::present(ItemShare* shares, QWidget* parent)
{
    auto* dialog = new SharedLinksDialog(shares, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* modal = VaultModal::forDialog(dialog, QStringLiteral("vaultSharedLinks"), CardWidth);
    if (!modal) {
        dialog->open();
        return dialog;
    }
    // The card brings the title and the close button, and its footer takes the hint and Share Text…. The content
    // lines up with the card's title.
    dialog->layout()->setContentsMargins(12, 4, 8, 4);
    dialog->m_closeButton->hide();
    modal->addFooterWidget(dialog->m_buttonRow, 1);
    modal->setInitialFocus(dialog->m_tabs);
    modal->open();
    return dialog;
}

QString SharedLinksDialog::describe(const ItemShare::Listing& share, const QDateTime& now)
{
    const auto opens = opensText(share);
    if (share.isEnded()) {
        return endReasonText(share.endReason) + " · " + opens;
    }
    const auto left = now.secsTo(share.expiresAt);
    QString expires;
    if (left <= 0) {
        expires = tr("Expired");
    } else if (left >= 82800) {
        // Days once at least 23 hours are left, so a 1-day link reads "1 day" right after it's made
        expires = tr("Expires in %n day(s)", "", static_cast<int>((left + 43200) / 86400));
    } else if (left >= 3600) {
        expires = tr("Expires in %n hour(s)", "", static_cast<int>(left / 3600));
    } else {
        expires = tr("Expires in a few minutes");
    }
    return expires + " · " + opens + (share.passcode ? " · " + tr("passcode") : QString());
}

void SharedLinksDialog::setListing(const QList<ItemShare::Listing>& shares)
{
    m_list = shares;
    m_loaded = true;
    fill();
}

void SharedLinksDialog::reload()
{
    if (!m_shares) {
        return;
    }
    QPointer<SharedLinksDialog> guard(this);
    m_shares->list([guard](const QList<ItemShare::Listing>& shares, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            guard->m_message->showMessage(error, VaultInfoBar::Error, VaultInfoBar::DisableAutoHide);
            return;
        }
        guard->setListing(shares);
    });
}

void SharedLinksDialog::fill()
{
    // Rows go later, as a click on one of their buttons may be what rebuilt the list
    while (m_rowsLayout->count() > 1) {
        auto* item = m_rowsLayout->takeAt(0);
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const auto now = Clock::currentDateTimeUtc();
    int active = 0;
    int ended = 0;
    QList<ItemShare::Listing> shown;
    for (const auto& share : m_list) {
        if (share.isEnded()) {
            ++ended;
        } else {
            ++active;
        }
        if (share.isEnded() == m_showEnded) {
            shown << share;
        }
    }
    m_tabs->setItemText(0, active ? tr("Active (%1)").arg(active) : tr("Active"));
    m_tabs->setItemText(1, ended ? tr("Ended (%1)").arg(ended) : tr("Ended"));

    for (int i = 0; i < shown.size(); ++i) {
        QList<QWidget*> widgets;
        if (i > 0) {
            widgets << new VaultDivider(m_rows);
        }
        widgets << makeRow(shown[i], now);
        for (auto* widget : widgets) {
            m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, widget);
            // Shown now rather than once the layout gets to it, so the list never flashes empty
            widget->show();
        }
    }

    const bool empty = shown.isEmpty();
    m_scroll->setVisible(!empty);
    m_empty->setVisible(empty);
    m_emptyAction->hide();
    if (!m_loaded) {
        m_emptyTitle->setText(m_shares ? tr("Loading links…") : tr("No links"));
        m_emptyText->clear();
    } else if (m_showEnded) {
        m_emptyTitle->setText(tr("No links ended"));
        m_emptyText->setText(tr("Links that ended stay here for 30 days."));
    } else {
        m_emptyTitle->setText(tr("No active links"));
        m_emptyText->setText(tr("Share an entry from its context menu, or some text with the button below."));
        if (ended > 0) {
            m_emptyAction->setText(tr("See %n that ended", "", ended));
            m_emptyAction->show();
        }
    }
    m_emptyText->setVisible(!m_emptyText->text().isEmpty());
}

QWidget* SharedLinksDialog::makeRow(const ItemShare::Listing& share, const QDateTime& now)
{
    const auto p = FluentTheme::palette();
    const bool active = !share.isEnded();
    const auto title = share.title.isEmpty() ? tr("(untitled)") : share.title;

    auto* row = new QWidget(m_rows);
    row->setObjectName("sharedLinkRow");
    row->setProperty("shareId", share.id);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(16, 12, 14, 12);
    layout->setSpacing(14);

    // A tile with the entry's first letter, or a text glyph for shared text; grey once the link ended
    auto* tile = new VaultPill(QString(), row);
    tile->setFixedSize(TileSize, TileSize);
    tile->setRadius(10);
    if (share.entryId.isEmpty()) {
        QFont glyph(Typography::FontFamily::FluentIcons);
        glyph.setPixelSize(16);
        tile->setFont(glyph);
        tile->setText(Typography::Icons::Document);
    } else {
        tile->setFont(Vault::headingFont(14));
        tile->setText(title.left(1).toUpper());
    }
    if (active) {
        tile->setColors(p.accentSubtle, p.accentText);
    } else {
        tile->setColors(p.inset, p.textSecondary);
    }
    layout->addWidget(tile);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* name = new QLabel(row);
    name->setObjectName("sharedLinkTitle");
    Vault::styleLabel(name, Typography::FontRole::BodyStrong, p.text);
    name->setText(name->fontMetrics().elidedText(title, Qt::ElideRight, 300));
    name->setToolTip(title);
    auto* meta = new QLabel(row);
    meta->setObjectName("sharedLinkMeta");
    Vault::styleLabel(meta, Typography::FontRole::Caption, p.textSecondary);
    meta->setText(active ? describe(share, now)
                         : opensText(share) + (share.passcode ? " · " + tr("passcode") : QString()));
    if (!share.openedAt.isEmpty()) {
        QStringList times;
        for (const auto& at : share.openedAt) {
            times << QLocale().toString(at.toLocalTime(), QLocale::ShortFormat);
        }
        meta->setToolTip(tr("Opened %1").arg(times.join(", ")));
    }
    text->addWidget(name);
    text->addWidget(meta);
    layout->addLayout(text, 1);

    if (active && m_confirming == share.id) {
        // Revoke asks in the row, not in a message box over the card
        auto* ask = new QLabel(tr("Stop this link?"), row);
        Vault::styleLabel(ask, Typography::FontRole::Caption, p.textSecondary);
        auto* keep = pillButton(tr("Keep"), row);
        keep->setObjectName("sharedLinkKeep");
        auto* confirm = pillButton(tr("Revoke"), row, true);
        confirm->setObjectName("sharedLinkConfirmRevoke");
        connect(keep, &QPushButton::clicked, this, [this] {
            m_confirming.clear();
            fill();
        });
        connect(confirm, &QPushButton::clicked, this, [this, id = share.id] { revoke(id); });
        layout->addWidget(ask);
        layout->addWidget(keep);
        layout->addWidget(confirm);
        return row;
    }

    auto* status = new VaultPill(active ? tr("Active") : endReasonText(share.endReason), row);
    status->setObjectName("sharedLinkStatus");
    if (active) {
        status->setColors(p.accentSubtle, p.accentText);
    } else {
        status->setColors(p.inset, p.textSecondary);
    }
    layout->addWidget(status, 0, Qt::AlignVCenter);
    if (active) {
        auto* revokeButton = pillButton(tr("Revoke"), row);
        revokeButton->setObjectName("sharedLinkRevoke");
        revokeButton->setAccessibleName(tr("Revoke the link to %1").arg(title));
        connect(revokeButton, &QPushButton::clicked, this, [this, id = share.id] {
            m_confirming = id;
            m_message->hide();
            fill();
        });
        layout->addWidget(revokeButton);
    }
    return row;
}

void SharedLinksDialog::revoke(const QString& id)
{
    m_confirming.clear();
    fill();
    if (!m_shares) {
        return;
    }
    QPointer<SharedLinksDialog> guard(this);
    m_shares->revoke(id, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->m_message->showMessage(error, VaultInfoBar::Error);
            return;
        }
        guard->m_message->showMessage(tr("The link was revoked. It no longer opens."), VaultInfoBar::Positive);
        guard->reload();
    });
}

void SharedLinksDialog::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_intro, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_hint, Typography::FontRole::Caption, p.textTertiary);
    Vault::styleLabel(m_emptyTitle, Typography::FontRole::BodyStrong, p.text);
    m_emptyTitle->setFont(Vault::headingFont(16));
    Vault::styleLabel(m_emptyText, Typography::FontRole::Body, p.textSecondary);
}

void SharedLinksDialog::shareText()
{
    // Over the main window it's another card, on top of this one
    auto* dialog = ShareLinkDialog::present(m_shares, {}, nullptr, this);
    connect(dialog, &QDialog::finished, this, &SharedLinksDialog::reload);
}
