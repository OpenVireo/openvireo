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
#include "gui/MessageBox.h"
#include "gui/MessageWidget.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
    QString endReasonText(const QString& reason)
    {
        if (reason == "opened") {
            return SharedLinksDialog::tr("Opened");
        }
        if (reason == "revoked") {
            return SharedLinksDialog::tr("Revoked");
        }
        if (reason == "passcode") {
            return SharedLinksDialog::tr("Too many wrong passcodes");
        }
        if (reason == "access_lost") {
            return SharedLinksDialog::tr("You no longer have the vault");
        }
        if (reason == "policy") {
            return SharedLinksDialog::tr("Turned off by your organization");
        }
        return SharedLinksDialog::tr("Expired");
    }
} // namespace

SharedLinksDialog::SharedLinksDialog(ItemShare* shares, QWidget* parent)
    : QDialog(parent)
    , m_shares(shares)
    , m_message(new MessageWidget(this))
    , m_tree(new QTreeWidget(this))
    , m_showEnded(new QCheckBox(tr("Show links that ended"), this))
{
    setWindowTitle(tr("Shared Links"));
    resize(640, 400);
    auto* layout = new QVBoxLayout(this);
    m_message->setHidden(true);
    layout->addWidget(m_message);
    auto* hint = new QLabel(tr("Links open a copy of an entry or some text in any browser, without an account. "
                               "Make one for an entry from its context menu."),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_tree->setHeaderLabels({tr("Shared"), tr("Status")});
    m_tree->setRootIsDecorated(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_showEnded);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* shareText = buttons->addButton(tr("Share Text…"), QDialogButtonBox::ActionRole);
    m_revoke = buttons->addButton(tr("Revoke"), QDialogButtonBox::ActionRole);
    m_revoke->setEnabled(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(shareText, &QPushButton::clicked, this, &SharedLinksDialog::shareText);
    connect(m_revoke, &QPushButton::clicked, this, &SharedLinksDialog::revokeSelected);
    connect(m_showEnded, &QCheckBox::toggled, this, &SharedLinksDialog::fill);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto* item = m_tree->currentItem();
        m_revoke->setEnabled(item && !item->data(0, Qt::UserRole + 1).toBool());
    });
    layout->addWidget(buttons);
    reload();
}

QString SharedLinksDialog::describe(const ItemShare::Listing& share, const QDateTime& now)
{
    const auto opens = share.maxOpens > 0 ? tr("opened %1 of %2").arg(share.opens).arg(share.maxOpens)
                                          : tr("%n open(s)", "", share.opens);
    if (share.isEnded()) {
        return endReasonText(share.endReason) + " · " + opens;
    }
    const auto left = now.secsTo(share.expiresAt);
    QString expires;
    if (left <= 0) {
        expires = tr("Expired");
    } else if (left >= 86400) {
        expires = tr("Expires in %n day(s)", "", static_cast<int>((left + 43200) / 86400));
    } else if (left >= 3600) {
        expires = tr("Expires in %n hour(s)", "", static_cast<int>(left / 3600));
    } else {
        expires = tr("Expires in a few minutes");
    }
    return expires + " · " + opens + (share.passcode ? " · " + tr("passcode") : QString());
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
            guard->m_message->showMessage(error, MessageWidget::Error, MessageWidget::DisableAutoHide);
            return;
        }
        guard->m_list = shares;
        guard->fill();
    });
}

void SharedLinksDialog::fill()
{
    m_tree->clear();
    const auto now = Clock::currentDateTimeUtc();
    int ended = 0;
    for (const auto& share : m_list) {
        if (share.isEnded()) {
            ++ended;
            if (!m_showEnded->isChecked()) {
                continue;
            }
        }
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, share.title.isEmpty() ? tr("(untitled)") : share.title);
        item->setText(1, describe(share, now));
        item->setData(0, Qt::UserRole, share.id);
        item->setData(0, Qt::UserRole + 1, share.isEnded());
        if (!share.openedAt.isEmpty()) {
            QStringList times;
            for (const auto& at : share.openedAt) {
                times << QLocale().toString(at.toLocalTime(), QLocale::ShortFormat);
            }
            item->setToolTip(1, tr("Opened %1").arg(times.join(", ")));
        }
    }
    m_showEnded->setText(ended ? tr("Show links that ended (%1)").arg(ended) : tr("Show links that ended"));
    m_revoke->setEnabled(false);
}

void SharedLinksDialog::revokeSelected()
{
    auto* item = m_tree->currentItem();
    if (!item || !m_shares) {
        return;
    }
    const auto answer = MessageBox::question(this,
                                             tr("Revoke Link"),
                                             tr("Revoke the link to %1? It stops opening at once.").arg(item->text(0)),
                                             MessageBox::Yes | MessageBox::Cancel,
                                             MessageBox::Cancel);
    if (answer != MessageBox::Yes) {
        return;
    }
    QPointer<SharedLinksDialog> guard(this);
    m_shares->revoke(item->data(0, Qt::UserRole).toString(), [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->m_message->showMessage(error, MessageWidget::Error);
            return;
        }
        guard->m_message->showMessage(tr("The link was revoked."), MessageWidget::Positive);
        guard->reload();
    });
}

void SharedLinksDialog::shareText()
{
    auto* dialog = new ShareLinkDialog(m_shares, {}, nullptr, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, &SharedLinksDialog::reload);
    dialog->open();
}
