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

#include "ShareLinkDialog.h"

#include "account/ItemShare.h"
#include "core/Entry.h"
#include "gui/Clipboard.h"
#include "gui/MessageWidget.h"
#include "gui/SquareSvgWidget.h"
#include "qrcode/QrCode.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace
{
    constexpr int Hour = 3600;
    constexpr int Day = 24 * Hour;

    // No look-alike characters, so a passcode read out over the phone is typed right
    QString newPasscode()
    {
        const QString alphabet("abcdefghjkmnpqrstuvwxyz23456789");
        QString chars;
        for (int i = 0; i < 12; ++i) {
            chars += alphabet.at(static_cast<int>(QRandomGenerator::system()->bounded(alphabet.size())));
        }
        return chars.mid(0, 4) + "-" + chars.mid(4, 4) + "-" + chars.mid(8);
    }
} // namespace

ShareLinkDialog::ShareLinkDialog(ItemShare* shares, const QString& vaultId, Entry* entry, QWidget* parent)
    : QDialog(parent)
    , m_shares(shares)
    , m_vaultId(vaultId)
    , m_entry(entry)
    , m_message(new MessageWidget(this))
    , m_pages(new QStackedWidget(this))
{
    setWindowTitle(entry ? tr("Share %1").arg(entry->title()) : tr("Share Text"));
    setMinimumWidth(480);
    m_blocked = shares ? shares->blockedReason(vaultId) : QString();

    auto* layout = new QVBoxLayout(this);
    m_message->setHidden(true);
    layout->addWidget(m_message);
    layout->addWidget(m_pages);

    // The form
    auto* form = new QWidget(m_pages);
    auto* formLayout = new QVBoxLayout(form);
    formLayout->setContentsMargins(0, 0, 0, 0);
    if (entry) {
        auto* hint = new QLabel(tr("Choose what to include. The link holds a copy: later changes to the entry don't reach it."), form);
        hint->setWordWrap(true);
        formLayout->addWidget(hint);
        const auto defaults = ItemShare::defaultFields();
        for (const auto& field : ItemShare::shareableFields(entry)) {
            auto* box = new QCheckBox(field.label, form);
            box->setProperty("fieldName", field.name);
            box->setChecked(defaults.contains(field.name));
            connect(box, &QCheckBox::toggled, this, &ShareLinkDialog::updateState);
            m_fieldBoxes << box;
            formLayout->addWidget(box);
        }
        if (m_fieldBoxes.isEmpty()) {
            formLayout->addWidget(new QLabel(tr("This entry has nothing to share."), form));
        }
        m_totpWarning = new QLabel(tr("Sharing the one-time code shares its secret: whoever opens the link can make "
                                      "codes for this account, even after the link ends."),
                                   form);
        m_totpWarning->setWordWrap(true);
        m_totpWarning->setStyleSheet("color: palette(highlight);");
        formLayout->addWidget(m_totpWarning);
    } else {
        m_title = new QLineEdit(form);
        m_title->setPlaceholderText(tr("Title (optional)"));
        m_title->setMaxLength(100);
        m_text = new QPlainTextEdit(form);
        m_text->setPlaceholderText(tr("Text to share"));
        connect(m_text, &QPlainTextEdit::textChanged, this, &ShareLinkDialog::updateState);
        formLayout->addWidget(m_title);
        formLayout->addWidget(m_text);
    }

    auto* options = new QFormLayout();
    m_expiry = new QComboBox(form);
    m_expiry->addItem(tr("1 hour"), Hour);
    m_expiry->addItem(tr("1 day"), Day);
    m_expiry->addItem(tr("7 days"), 7 * Day);
    m_expiry->addItem(tr("14 days"), 14 * Day);
    m_expiry->addItem(tr("30 days"), 30 * Day);
    m_expiry->setCurrentIndex(2);
    options->addRow(tr("Link expires after:"), m_expiry);
    m_opens = new QComboBox(form);
    m_opens->addItem(tr("Any number of times"), 0);
    m_opens->addItem(tr("Once"), 1);
    m_opens->addItem(tr("5 times"), 5);
    options->addRow(tr("Can be opened:"), m_opens);
    m_usePasscode = new QCheckBox(tr("Require a passcode"), form);
    options->addRow(m_usePasscode);
    auto* passcodeRow = new QHBoxLayout();
    m_passcode = new QLineEdit(form);
    m_passcode->setToolTip(tr("At least 6 characters. Send it separately from the link."));
    auto* regenerate = new QPushButton(tr("New"), form);
    passcodeRow->addWidget(m_passcode, 1);
    passcodeRow->addWidget(regenerate);
    options->addRow(tr("Passcode:"), passcodeRow);
    formLayout->addLayout(options);
    formLayout->addStretch(1);
    connect(m_usePasscode, &QCheckBox::toggled, this, [this, regenerate](bool on) {
        if (on && m_passcode->text().isEmpty()) {
            m_passcode->setText(newPasscode());
        }
        regenerate->setEnabled(on);
        updateState();
    });
    connect(regenerate, &QPushButton::clicked, this, [this] { m_passcode->setText(newPasscode()); });
    connect(m_passcode, &QLineEdit::textChanged, this, &ShareLinkDialog::updateState);
    regenerate->setEnabled(false);
    m_pages->addWidget(form);

    // The link, once made
    auto* result = new QWidget(m_pages);
    auto* resultLayout = new QVBoxLayout(result);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    m_linkInfo = new QLabel(result);
    m_linkInfo->setWordWrap(true);
    m_link = new QLineEdit(result);
    m_link->setReadOnly(true);
    m_link->setObjectName("shareLinkUrl");
    auto* copyRow = new QHBoxLayout();
    auto* copyLink = new QPushButton(tr("Copy Link"), result);
    auto* copyPasscode = new QPushButton(tr("Copy Passcode"), result);
    copyPasscode->setObjectName("shareLinkCopyPasscode");
    copyRow->addWidget(copyLink);
    copyRow->addWidget(copyPasscode);
    copyRow->addStretch(1);
    m_qr = new SquareSvgWidget(result);
    auto* manage = new QLabel(tr("You can see when it's opened, and revoke it, from Account → Shared Links."), result);
    manage->setWordWrap(true);
    resultLayout->addWidget(m_linkInfo);
    resultLayout->addWidget(m_link);
    resultLayout->addLayout(copyRow);
    resultLayout->addWidget(m_qr, 0, Qt::AlignHCenter);
    resultLayout->addWidget(manage);
    connect(copyLink, &QPushButton::clicked, this, [this] {
        clipboard()->setText(m_link->text());
        m_message->showMessage(tr("Link copied."), MessageWidget::Positive);
    });
    connect(copyPasscode, &QPushButton::clicked, this, [this] {
        clipboard()->setText(m_passcode->text());
        m_message->showMessage(tr("Passcode copied."), MessageWidget::Positive);
    });
    m_pages->addWidget(result);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_createButton = buttons->addButton(tr("Make Link"), QDialogButtonBox::AcceptRole);
    m_createButton->setDefault(true);
    connect(m_createButton, &QPushButton::clicked, this, &ShareLinkDialog::create);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (!m_blocked.isEmpty()) {
        m_message->showMessage(m_blocked, MessageWidget::Warning, MessageWidget::DisableAutoHide);
    }
    updateState();
}

void ShareLinkDialog::updateState()
{
    bool hasContent = false;
    bool sharesTotp = false;
    if (m_entry) {
        for (auto* box : m_fieldBoxes) {
            hasContent |= box->isChecked();
            sharesTotp |= box->isChecked() && box->property("fieldName").toString() == "otp";
        }
        m_totpWarning->setVisible(sharesTotp);
    } else if (m_text) {
        hasContent = !m_text->toPlainText().trimmed().isEmpty();
    }
    m_passcode->setEnabled(m_usePasscode->isChecked());
    const bool passcodeOk = !m_usePasscode->isChecked() || m_passcode->text().size() >= 6;
    m_createButton->setEnabled(m_blocked.isEmpty() && hasContent && passcodeOk && m_shares);
}

void ShareLinkDialog::create()
{
    if (!m_shares || !m_createButton->isEnabled()) {
        return;
    }
    QJsonObject payload;
    ItemShare::Options options;
    if (m_entry) {
        QStringList names;
        for (auto* box : m_fieldBoxes) {
            if (box->isChecked()) {
                names << box->property("fieldName").toString();
            }
        }
        payload = ItemShare::entryPayload(m_entry, names);
        options.vaultId = m_vaultId;
        options.entryId = QString::fromLatin1(m_entry->uuid().toRfc4122().toHex());
    } else {
        payload = ItemShare::textPayload(m_text->toPlainText(), m_title->text().trimmed());
    }
    options.expiresIn = m_expiry->currentData().toInt();
    options.maxOpens = m_opens->currentData().toInt();
    options.passcode = m_usePasscode->isChecked() ? m_passcode->text() : QString();

    m_createButton->setEnabled(false);
    m_createButton->setText(tr("Making Link…"));
    QPointer<ShareLinkDialog> guard(this);
    m_shares->create(payload, options, [guard](const ItemShare::Created& share, const QString& error) {
        if (!guard) {
            return;
        }
        guard->m_createButton->setText(tr("Make Link"));
        if (!error.isEmpty()) {
            guard->m_message->showMessage(error, MessageWidget::Error, MessageWidget::DisableAutoHide);
            guard->updateState();
            return;
        }
        guard->showLink(share.url, QLocale().toString(share.expiresAt.toLocalTime(), QLocale::ShortFormat));
    });
}

void ShareLinkDialog::showLink(const QString& url, const QString& expires)
{
    const auto opens = m_opens->currentData().toInt();
    auto info = tr("Anyone with this link can open it until %1.").arg(expires);
    if (opens == 1) {
        info += " " + tr("It opens once.");
    } else if (opens > 1) {
        info += " " + tr("It opens %1 times.").arg(opens);
    }
    info += " " + (m_usePasscode->isChecked() ? tr("Send the passcode another way than the link.")
                                               : tr("Send it the way you'd send the secret itself."));
    m_linkInfo->setText(info);
    m_link->setText(url);
    findChild<QPushButton*>("shareLinkCopyPasscode")->setVisible(m_usePasscode->isChecked());
    const QrCode qrc(url);
    if (qrc.isValid()) {
        QBuffer buffer;
        qrc.writeSvg(&buffer, logicalDpiX());
        m_qr->load(buffer.data());
        const auto size = static_cast<int>(logicalDpiX() * 2);
        m_qr->setMinimumSize(size, size);
    }
    m_message->hide();
    m_pages->setCurrentIndex(1);
    m_createButton->hide();
    if (auto* box = findChild<QDialogButtonBox*>()) {
        box->button(QDialogButtonBox::Cancel)->setText(tr("Close"));
    }
}
