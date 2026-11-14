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

#include "OrgAdminWidgets.h"
#include "account/ItemShare.h"
#include "core/Entry.h"
#include "gui/Clipboard.h"
#include "gui/SquareSvgWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultSettingsPage.h"
#include "gui/vault/VaultWidgets.h"
#include "qrcode/QrCode.h"

#include <components/status_info/InfoBar.h>

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace
{
    constexpr int Hour = 3600;
    constexpr int Day = 24 * Hour;

    // Width of the Fluent window, and of the card over the main window
    constexpr int VaultDialogWidth = 560;
    // WinUI's content dialog padding, for the dialog when it shows as a window of its own
    constexpr int VaultDialogPadding = 24;
    constexpr int VaultQrSize = 168;
    // How long a confirmation, such as "Link copied.", stays in the Fluent layout
    constexpr int VaultMessageTimeout = 3000;

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

    // A Fluent control in the Vault Index layout, the stock Qt one otherwise
    template <typename Fluent, typename Classic, typename... Args> Classic* make(bool vault, Args&&... args)
    {
        if (vault) {
            return new Fluent(std::forward<Args>(args)...);
        }
        return new Classic(std::forward<Args>(args)...);
    }

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
} // namespace

ShareLinkDialog::ShareLinkDialog(ItemShare* shares, const QString& vaultId, Entry* entry, QWidget* parent)
    : QDialog(parent)
    , m_shares(shares)
    , m_vaultId(vaultId)
    , m_entry(entry)
    , m_vault(FluentTheme::vaultLayoutEnabled())
{
    setWindowTitle(entry ? tr("Share %1").arg(entry->title()) : tr("Share Text"));
    m_blocked = shares ? shares->blockedReason(vaultId) : QString();

    if (m_vault) {
        setupVaultLayout();
    } else {
        setupClassicLayout();
    }

    connect(m_usePasscode, &QCheckBox::toggled, this, [this](bool on) {
        if (on && m_passcode->text().isEmpty()) {
            m_passcode->setText(newPasscode());
        }
        // The Fluent layout shows the passcode only while it's asked for; the classic one greys it out
        if (m_vault) {
            m_passcode->setVisible(on);
            m_regenerate->setVisible(on);
            refitWindow();
        } else {
            m_regenerate->setEnabled(on);
        }
        updateState();
    });
    connect(m_regenerate, &QPushButton::clicked, this, [this] { m_passcode->setText(newPasscode()); });
    connect(m_passcode, &QLineEdit::textChanged, this, &ShareLinkDialog::updateState);
    connect(m_createButton, &QPushButton::clicked, this, &ShareLinkDialog::create);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_copyLink, &QPushButton::clicked, this, [this] {
        clipboard()->setText(m_link->text());
        showMessage(tr("Link copied."), MessageWidget::Positive);
    });
    connect(m_copyPasscode, &QPushButton::clicked, this, [this] {
        clipboard()->setText(m_passcode->text());
        showMessage(tr("Passcode copied."), MessageWidget::Positive);
    });

    if (!m_blocked.isEmpty()) {
        showMessage(m_blocked, MessageWidget::Warning, true);
    }
    updateState();
}

ShareLinkDialog* ShareLinkDialog::present(ItemShare* shares, const QString& vaultId, Entry* entry, QWidget* parent)
{
    auto* dialog = new ShareLinkDialog(shares, vaultId, entry, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (dialog->showAsModal()) {
        dialog->presentAsModal();
    } else {
        if (dialog->m_vault) {
            dialog->fitWindow();
        }
        dialog->open();
    }
    return dialog;
}

void ShareLinkDialog::addFieldChoices(QWidget* parent, QLayout* layout)
{
    const auto defaults = ItemShare::defaultFields();
    for (const auto& field : ItemShare::shareableFields(m_entry)) {
        auto* box = make<VaultCheckBox, QCheckBox>(m_vault, field.label, parent);
        box->setProperty("fieldName", field.name);
        box->setChecked(defaults.contains(field.name));
        connect(box, &QCheckBox::toggled, this, &ShareLinkDialog::updateState);
        m_fieldBoxes << box;
        layout->addWidget(box);
    }
    if (m_fieldBoxes.isEmpty()) {
        auto* nothing = m_vault ? OrgAdmin::label(parent, Typography::FontRole::Body, true) : new QLabel(parent);
        nothing->setText(tr("This entry has nothing to share."));
        layout->addWidget(nothing);
    }
}

void ShareLinkDialog::addTextFields(QWidget* parent, QLayout* layout)
{
    m_title = make<VaultLineEdit, QLineEdit>(m_vault, parent);
    m_title->setPlaceholderText(tr("Title (optional)"));
    m_title->setMaxLength(100);
    m_text = make<VaultPlainTextEdit, QPlainTextEdit>(m_vault, parent);
    m_text->setPlaceholderText(tr("Text to share"));
    if (m_vault) {
        m_text->setMinimumHeight(140);
    }
    connect(m_text, &QPlainTextEdit::textChanged, this, &ShareLinkDialog::updateState);
    layout->addWidget(m_title);
    layout->addWidget(m_text);
}

void ShareLinkDialog::addOptions(QWidget* parent)
{
    m_expiry = make<VaultComboBox, QComboBox>(m_vault, parent);
    m_expiry->addItem(tr("1 hour"), Hour);
    m_expiry->addItem(tr("1 day"), Day);
    m_expiry->addItem(tr("7 days"), 7 * Day);
    m_expiry->addItem(tr("14 days"), 14 * Day);
    m_expiry->addItem(tr("30 days"), 30 * Day);
    m_expiry->setCurrentIndex(2);
    m_opens = make<VaultComboBox, QComboBox>(m_vault, parent);
    m_opens->addItem(tr("Any number of times"), 0);
    m_opens->addItem(tr("Once"), 1);
    m_opens->addItem(tr("5 times"), 5);
    m_usePasscode = make<VaultCheckBox, QCheckBox>(m_vault, tr("Require a passcode"), parent);
    m_passcode = make<VaultLineEdit, QLineEdit>(m_vault, parent);
    m_passcode->setObjectName("shareLinkPasscode");
    m_passcode->setToolTip(tr("At least 6 characters. Send it separately from the link."));
    if (m_vault) {
        auto* regenerate = new VaultButton(parent);
        regenerate->setFluentLayout(fluent::basicinput::Button::IconOnly);
        regenerate->setIconGlyph(Typography::Icons::Refresh);
        regenerate->setToolTip(tr("New passcode"));
        regenerate->setAccessibleName(tr("New passcode"));
        m_regenerate = regenerate;
    } else {
        m_regenerate = new QPushButton(tr("New"), parent);
        m_regenerate->setEnabled(false);
    }
}

void ShareLinkDialog::setupClassicLayout()
{
    setMinimumWidth(480);
    auto* layout = new QVBoxLayout(this);
    m_message = new MessageWidget(this);
    m_message->setHidden(true);
    layout->addWidget(m_message);

    // The form
    m_formPage = new QWidget(this);
    auto* formLayout = new QVBoxLayout(m_formPage);
    formLayout->setContentsMargins(0, 0, 0, 0);
    if (m_entry) {
        auto* hint = new QLabel(tr("Choose what to include. The link holds a copy: later changes to the entry don't "
                                   "reach it."),
                                m_formPage);
        hint->setWordWrap(true);
        formLayout->addWidget(hint);
        addFieldChoices(m_formPage, formLayout);
        auto* warning = new QLabel(tr("Sharing the one-time code shares its secret: whoever opens the link can make "
                                      "codes for this account, even after the link ends."),
                                   m_formPage);
        warning->setWordWrap(true);
        warning->setStyleSheet("color: palette(highlight);");
        formLayout->addWidget(warning);
        m_totpWarning = warning;
    } else {
        addTextFields(m_formPage, formLayout);
    }

    addOptions(m_formPage);
    auto* options = new QFormLayout();
    options->addRow(tr("Link expires after:"), m_expiry);
    options->addRow(tr("Can be opened:"), m_opens);
    options->addRow(m_usePasscode);
    auto* passcodeRow = new QHBoxLayout();
    passcodeRow->addWidget(m_passcode, 1);
    passcodeRow->addWidget(m_regenerate);
    options->addRow(tr("Passcode:"), passcodeRow);
    formLayout->addLayout(options);
    formLayout->addStretch(1);
    layout->addWidget(m_formPage);

    // The link, once made
    m_resultPage = new QWidget(this);
    auto* resultLayout = new QVBoxLayout(m_resultPage);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    m_linkInfo = new QLabel(m_resultPage);
    m_linkInfo->setWordWrap(true);
    m_link = new QLineEdit(m_resultPage);
    m_link->setReadOnly(true);
    m_link->setObjectName("shareLinkUrl");
    auto* copyRow = new QHBoxLayout();
    m_copyLink = new QPushButton(tr("Copy Link"), m_resultPage);
    m_copyPasscode = new QPushButton(tr("Copy Passcode"), m_resultPage);
    m_copyPasscode->setObjectName("shareLinkCopyPasscode");
    copyRow->addWidget(m_copyLink);
    copyRow->addWidget(m_copyPasscode);
    copyRow->addStretch(1);
    m_qr = new SquareSvgWidget(m_resultPage);
    auto* manage = new QLabel(tr("You can see when it's opened, and revoke it, from Account → Shared Links."),
                              m_resultPage);
    manage->setWordWrap(true);
    resultLayout->addWidget(m_linkInfo);
    resultLayout->addWidget(m_link);
    resultLayout->addLayout(copyRow);
    resultLayout->addWidget(m_qr, 0, Qt::AlignHCenter);
    resultLayout->addWidget(manage);
    m_resultPage->hide();
    layout->addWidget(m_resultPage);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_closeButton = buttons->button(QDialogButtonBox::Cancel);
    m_createButton = buttons->addButton(tr("Make Link"), QDialogButtonBox::AcceptRole);
    m_createButton->setDefault(true);
    layout->addWidget(buttons);
}

// In the Vault Index layout, the choices read as Fluent settings cards: the fields to include as check boxes on a
// card, then the link's settings, with the passcode showing below its switch while it's on. The made link has a card
// with the link and its copy button, the passcode, and the QR code. Over the main window the dialog is the content
// of a modal card, which provides the title and the buttons; over another dialog it is a Fluent window of its own,
// with a title and commands laid out like a content dialog's.
void ShareLinkDialog::setupVaultLayout()
{
    using fluent::status_info::InfoBar;

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* title = OrgAdmin::label(this, Typography::FontRole::Subtitle);
    title->setObjectName("shareLinkTitle");
    title->setText(windowTitle());
    title->setContentsMargins(VaultDialogPadding, VaultDialogPadding, VaultDialogPadding, 0);
    outer->addWidget(title);
    m_vaultTitle = title;

    // The pages' cards keep a little room on the right for a scroll bar, so the body's margins even that out
    auto* body = new QWidget(this);
    body->setObjectName("shareLinkBody");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(VaultDialogPadding - 4, 8, VaultDialogPadding - 12, 0);
    bodyLayout->setSpacing(0);
    outer->addWidget(body, 1);

    // Problems and confirmations, above either page
    m_infoBar = new InfoBar(body);
    m_infoBar->setObjectName("shareLinkInfoBar");
    m_infoBar->setIsClosable(false);
    m_infoBar->setIsOpen(true);
    m_infoBar->setSizePolicy(QSizePolicy::Expanding, m_infoBar->sizePolicy().verticalPolicy());
    m_infoBar->setContentsMargins(4, 4, 12, 0);
    m_infoBar->hide();
    bodyLayout->addWidget(m_infoBar);

    // The form
    m_formPage = new QWidget(body);
    m_formPage->setObjectName("shareLinkForm");
    {
        VaultSettingsPage page(m_formPage, {});
        if (m_entry) {
            auto* include = page.addSection(tr("Include"));
            auto* hint = OrgAdmin::label(m_formPage, Typography::FontRole::Body, true);
            hint->setText(tr("The link holds a copy: later changes to the entry don't reach it."));
            include->addHeaderWidget(hint, {});
            auto* choices = new QWidget(m_formPage);
            auto* choicesLayout = new QVBoxLayout(choices);
            choicesLayout->setContentsMargins(0, 4, 0, 4);
            choicesLayout->setSpacing(8);
            addFieldChoices(choices, choicesLayout);
            include->addWidget(choices);

            auto* warning = new InfoBar(m_formPage);
            warning->setObjectName("shareLinkTotpWarning");
            warning->setSeverity(InfoBar::Warning);
            warning->setTitle(tr("Shares the one-time code's secret"));
            warning->setMessage(tr("Whoever opens the link can make codes for this account, even after the link "
                                   "ends."));
            warning->setIsClosable(false);
            warning->setIsOpen(true);
            warning->setSizePolicy(QSizePolicy::Expanding, warning->sizePolicy().verticalPolicy());
            page.addWidget(warning);
            m_totpWarning = warning;
        } else {
            auto* content = page.addSection(tr("Text"));
            auto* fields = new QWidget(m_formPage);
            auto* fieldsLayout = new QVBoxLayout(fields);
            fieldsLayout->setContentsMargins(0, 4, 0, 4);
            fieldsLayout->setSpacing(8);
            addTextFields(fields, fieldsLayout);
            content->addWidget(fields);
        }

        addOptions(m_formPage);
        auto* options = page.addSection(tr("Link"));
        options->addSetting(tr("Expires after"), m_expiry);
        options->addSetting(tr("Can be opened"), m_opens);
        auto* passcode = options->addToggle(
            m_usePasscode, tr("Whoever opens the link types it in first. Send it another way than the link."));
        passcode->setCollapsible(false);
        passcode->addSetting(tr("Passcode"), QWidgetList{m_passcode, m_regenerate}, tr("At least 6 characters"));
        m_passcode->hide();
        m_regenerate->hide();
        page.finish();
    }
    bodyLayout->addWidget(m_formPage);

    // The link, once made
    m_resultPage = new QWidget(body);
    m_resultPage->setObjectName("shareLinkResult");
    {
        VaultSettingsPage page(m_resultPage, {});
        m_linkInfo = OrgAdmin::label(m_resultPage, Typography::FontRole::Body);
        m_linkInfo->setObjectName("shareLinkInfo");
        page.addWidget(m_linkInfo);

        auto* link = page.addSection(tr("Link"));
        auto* linkRow = new QWidget(m_resultPage);
        auto* linkLayout = new QHBoxLayout(linkRow);
        linkLayout->setContentsMargins(0, 4, 0, 4);
        linkLayout->setSpacing(8);
        m_link = new VaultLineEdit(linkRow);
        m_link->setReadOnly(true);
        m_link->setObjectName("shareLinkUrl");
        m_link->setSizePolicy(QSizePolicy::Expanding, m_link->sizePolicy().verticalPolicy());
        auto* copyLink = new VaultButton(tr("Copy Link"), linkRow);
        copyLink->setObjectName("shareLinkCopy");
        copyLink->setFluentStyle(fluent::basicinput::Button::Accent);
        m_copyLink = copyLink;
        linkLayout->addWidget(m_link, 1);
        linkLayout->addWidget(m_copyLink);
        link->addWidget(linkRow);

        auto* passcodeCaption = new QLabel(tr("Passcode"), m_resultPage);
        m_passcodeShown = new QLabel(m_resultPage);
        m_passcodeShown->setObjectName("shareLinkPasscodeShown");
        m_passcodeShown->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_copyPasscode = new VaultButton(tr("Copy Passcode"), m_resultPage);
        m_copyPasscode->setObjectName("shareLinkCopyPasscode");
        link->addSetting(VaultSettingsCaption(passcodeCaption, m_passcodeShown), m_copyPasscode);

        auto* qr = page.addSection(tr("QR code"));
        auto* qrBox = new QWidget(m_resultPage);
        auto* qrLayout = new QVBoxLayout(qrBox);
        qrLayout->setContentsMargins(0, 8, 0, 8);
        qrLayout->setSpacing(8);
        m_qr = new SquareSvgWidget(qrBox);
        m_qr->setFixedSize(VaultQrSize, VaultQrSize);
        qrLayout->addWidget(m_qr, 0, Qt::AlignHCenter);
        auto* scan = OrgAdmin::label(qrBox, Typography::FontRole::Caption, true);
        scan->setText(tr("Scan it with a phone to open the link there."));
        scan->setAlignment(Qt::AlignHCenter);
        qrLayout->addWidget(scan);
        qr->addWidget(qrBox);

        auto* manage = OrgAdmin::label(m_resultPage, Typography::FontRole::Caption, true);
        manage->setText(tr("You can see when it's opened, and revoke it, from Account → Shared Links."));
        page.addWidget(manage);
        page.finish();
    }
    m_resultPage->hide();
    bodyLayout->addWidget(m_resultPage);

    // The commands, for the window of its own
    auto* commands = new CommandBar(this);
    commands->setObjectName("shareLinkCommands");
    auto* commandsLayout = new QHBoxLayout(commands);
    commandsLayout->setContentsMargins(VaultDialogPadding, VaultDialogPadding, VaultDialogPadding, VaultDialogPadding);
    commandsLayout->setSpacing(8);
    auto* create = new VaultButton(tr("Make Link"), commands);
    create->setObjectName("shareLinkMake");
    create->setFluentStyle(fluent::basicinput::Button::Accent);
    create->setDefault(true);
    auto* close = new VaultButton(tr("Cancel"), commands);
    close->setObjectName("shareLinkCancel");
    commandsLayout->addWidget(create, 1);
    commandsLayout->addWidget(close, 1);
    outer->addWidget(commands);
    m_vaultCommands = commands;
    m_createButton = create;
    m_closeButton = close;

    // The window is a content dialog's card: the layer fill, with the commands on the canvas below
    Vault::followTheme(this, [this] {
        auto pal = palette();
        pal.setColor(QPalette::Window, FluentTheme::palette().layer);
        setPalette(pal);
    });
    setAutoFillBackground(true);
    setFixedWidth(VaultDialogWidth);
}

bool ShareLinkDialog::showAsModal() const
{
    auto* parent = parentWidget();
    if (!m_vault || !parent) {
        return false;
    }
    // Over another dialog, such as Shared Links, a card would sit inside that dialog, so it is a window instead
    auto* window = parent->window();
    return !qobject_cast<QDialog*>(window) && window->isVisible() && !window->isMinimized();
}

void ShareLinkDialog::presentAsModal()
{
    // The modal card provides the title, the padding and the buttons
    m_vaultTitle->hide();
    m_vaultCommands->hide();
    findChild<QWidget*>("shareLinkBody")->layout()->setContentsMargins(0, 0, 0, 0);
    setAutoFillBackground(false);
    setMinimumWidth(0);
    setMaximumWidth(QWIDGETSIZE_MAX);
    setWindowFlags(Qt::Widget);

    auto* modal = new VaultModal(parentWidget()->window(), this, windowTitle(), QStringLiteral("vaultShareLink"));
    modal->setMaximumCardWidth(VaultDialogWidth);
    modal->setFitToContent(true);
    if (!m_fieldBoxes.isEmpty()) {
        modal->setInitialFocus(m_fieldBoxes.first());
    } else if (m_text) {
        modal->setInitialFocus(m_text);
    }

    // The card's footer takes the place of the dialog's own commands
    m_closeButton = modal->addFooterButton(tr("Cancel"), false, QStringLiteral("Cancel"));
    m_createButton = modal->addFooterButton(tr("Make Link"), true, QStringLiteral("Make"));
    connect(m_createButton, &QPushButton::clicked, this, &ShareLinkDialog::create);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    // Return makes the link, and once it's made closes the card
    connect(modal, &VaultModal::accepted, this, [this] {
        if (m_resultPage->isVisible()) {
            reject();
        } else {
            create();
        }
    });
    connect(modal, &VaultModal::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, modal, [modal] {
        modal->close();
        modal->deleteLater();
    });
    updateState();
    modal->open();
}

void ShareLinkDialog::showMessage(const QString& text, MessageWidget::MessageType type, bool persistent)
{
    if (!m_infoBar) {
        if (persistent) {
            m_message->showMessage(text, type, MessageWidget::DisableAutoHide);
        } else {
            m_message->showMessage(text, type);
        }
        return;
    }
    using fluent::status_info::InfoBar;
    switch (type) {
    case MessageWidget::Positive:
        m_infoBar->setSeverity(InfoBar::Success);
        break;
    case MessageWidget::Warning:
        m_infoBar->setSeverity(InfoBar::Warning);
        break;
    case MessageWidget::Error:
        m_infoBar->setSeverity(InfoBar::Error);
        break;
    default:
        m_infoBar->setSeverity(InfoBar::Informational);
        break;
    }
    m_infoBar->setMessage(text);
    m_infoBar->show();
    refitWindow();

    // A confirmation goes away by itself, unless another message took its place
    const int generation = m_infoBar->property("generation").toInt() + 1;
    m_infoBar->setProperty("generation", generation);
    if (!persistent) {
        QTimer::singleShot(VaultMessageTimeout, m_infoBar, [bar = m_infoBar, generation] {
            if (bar->property("generation").toInt() == generation) {
                bar->hide();
            }
        });
        QTimer::singleShot(VaultMessageTimeout + 10, this, &ShareLinkDialog::refitWindow);
    }
}

void ShareLinkDialog::hideMessage()
{
    if (m_infoBar) {
        m_infoBar->setProperty("generation", m_infoBar->property("generation").toInt() + 1);
        m_infoBar->hide();
        refitWindow();
    } else {
        m_message->hide();
    }
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
            guard->showMessage(error, MessageWidget::Error, true);
            guard->updateState();
            return;
        }
        guard->showLink(share.url, QLocale().toString(share.expiresAt.toLocalTime(), QLocale::ShortFormat));
    });
}

void ShareLinkDialog::showLink(const QString& url, const QString& expires)
{
    const auto opens = m_opens->currentData().toInt();
    const bool passcode = m_usePasscode->isChecked();
    auto info = tr("Anyone with this link can open it until %1.").arg(expires);
    if (opens == 1) {
        info += " " + tr("It opens once.");
    } else if (opens > 1) {
        info += " " + tr("It opens %1 times.").arg(opens);
    }
    info += " " + (passcode ? tr("Send the passcode another way than the link.")
                            : tr("Send it the way you'd send the secret itself."));
    m_linkInfo->setText(info);
    m_link->setText(url);
    m_link->setCursorPosition(0);
    if (m_passcodeShown) {
        m_passcodeShown->setText(m_passcode->text());
    }
    m_copyPasscode->setVisible(passcode);

    const QrCode qrc(url);
    if (qrc.isValid()) {
        QBuffer buffer;
        qrc.writeSvg(&buffer, logicalDpiX());
        m_qr->load(buffer.data());
        if (!m_vault) {
            const auto size = static_cast<int>(logicalDpiX() * 2);
            m_qr->setMinimumSize(size, size);
        }
    }
    hideMessage();
    m_formPage->hide();
    m_resultPage->show();
    m_createButton->hide();
    if (m_vault) {
        // Done is all that's left to do
        m_closeButton->setText(tr("Done"));
        if (auto* button = qobject_cast<fluent::basicinput::Button*>(m_closeButton)) {
            button->setFluentStyle(fluent::basicinput::Button::Accent);
        }
        m_closeButton->setDefault(true);
        m_copyLink->setFocus();
    } else {
        m_closeButton->setText(tr("Close"));
    }
    refitWindow();
}

void ShareLinkDialog::refitWindow()
{
    // Once its layout has taken in what showed or hid
    if (m_vault && !m_vaultCommands->isHidden()) {
        QTimer::singleShot(0, this, &ShareLinkDialog::fitWindow);
    }
}

void ShareLinkDialog::fitWindow()
{
    layout()->activate();
    const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(VaultDialogWidth)
                                                     : sizeHint().height();
    // Fixed, as opening the dialog would otherwise grow it past its content
    setFixedSize(VaultDialogWidth, height);
}
