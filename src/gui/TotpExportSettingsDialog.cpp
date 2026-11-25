/*
 *  Copyright (C) 2017 KeePassXC Team <team@keepassxc.org>
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

#include "TotpExportSettingsDialog.h"

#include "core/Totp.h"
#include "gui/Clipboard.h"
#include "gui/MainWindow.h"
#include "gui/MessageBox.h"
#include "gui/SquareSvgWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultInfoBar.h"
#include "gui/vault/VaultWidgets.h"
#include "qrcode/QrCode.h"

#include <QBoxLayout>
#include <QBuffer>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>

namespace
{
    constexpr int CardWidth = 420;
    constexpr int CloseAfterSeconds = 45;

    // The QR code's backdrop: white whatever the theme, since scanners expect dark modules on light
    class QrBackdrop : public QStackedWidget
    {
    public:
        using QStackedWidget::QStackedWidget;

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 20, 20);
            painter.fillPath(path, Qt::white);
            painter.setPen(QPen(FluentTheme::palette().stroke, 1));
            painter.drawPath(path);
        }
    };
} // namespace

TotpExportSettingsDialog::TotpExportSettingsDialog(DatabaseWidget* parent, Entry* entry)
    : VaultDialog(parent)
    , m_timer(new QTimer(this))
    , m_verticalLayout(new QVBoxLayout())
    , m_totpSvgContainerWidget(new QrBackdrop())
    , m_totpSvgWidget(new SquareSvgWidget(m_totpSvgContainerWidget))
    , m_intro(new QLabel())
    , m_countDown(new QLabel())
    , m_warning(new VaultInfoBar())
    , m_buttonBox(new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Ok))
{
    setObjectName("entryQrCodeWidget");
    setWindowTitle(tr("Set up on another device"));
    setNamePrefix(QStringLiteral("vaultTotpQr"));
    setMaximumDialogWidth(CardWidth);
    m_totpSvgContainerWidget->setContentsMargins(16, 16, 16, 16);
    m_totpSvgContainerWidget->addWidget(m_totpSvgWidget);

    m_intro->setWordWrap(true);
    m_intro->setTextFormat(Qt::PlainText);
    m_intro->setText(tr("Scan it with an authenticator app to add %1.").arg(entry->title()));
    m_countDown->setAlignment(Qt::AlignCenter);

    m_verticalLayout->setSpacing(14);
    m_verticalLayout->addWidget(m_intro);
    m_verticalLayout->addWidget(m_warning);
    m_verticalLayout->addWidget(m_totpSvgContainerWidget, 0, Qt::AlignHCenter);
    m_verticalLayout->addWidget(m_countDown);
    m_verticalLayout->addWidget(m_buttonBox);

    setLayout(m_verticalLayout);
    setAttribute(Qt::WA_DeleteOnClose);

    connect(m_buttonBox, SIGNAL(rejected()), SLOT(close()));
    connect(m_buttonBox, SIGNAL(accepted()), SLOT(copyToClipboard()));
    connect(m_timer, SIGNAL(timeout()), SLOT(autoClose()));

    new QShortcut(QKeySequence(QKeySequence::Copy), this, SLOT(copyToClipboard()));

    m_buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Copy setup link"));
    m_buttonBox->button(QDialogButtonBox::Ok)->setDefault(true);
    setInitialFocus(m_buttonBox->button(QDialogButtonBox::Ok));

    m_secTillClose = CloseAfterSeconds + 1;
    autoClose();
    m_timer->start(1000);

    const auto totpSettings = entry->totpSettings();
    m_warning->setHidden(true);
    if (Totp::hasCustomSettings(totpSettings) || !totpSettings->encoder.shortName.isEmpty()) {
        m_warning->showMessage(tr("These TOTP settings are custom and may not work in every authenticator.",
                                  "TOTP QR code dialog warning"),
                               VaultInfoBar::Warning,
                               VaultInfoBar::DisableAutoHide);
    }
    Vault::followTheme(this, [this] { applyColors(); });

    m_totpUri = Totp::writeSettings(entry->totpSettings(), entry->title(), entry->username(), true);
    const QrCode qrc(m_totpUri);

    if (qrc.isValid()) {
        QBuffer buffer;
        qrc.writeSvg(&buffer, logicalDpiX());
        m_totpSvgWidget->load(buffer.data());
        const auto minsize = static_cast<int>(logicalDpiX() * 2.5);
        m_totpSvgWidget->setMinimumSize(minsize, minsize);
        // A square of white around the code, however tall the card gets
        m_totpSvgContainerWidget->setFixedSize(minsize + 32, minsize + 32);
    } else {
        MessageBox::warning(parent, {}, tr("There was an error creating the QR code."));
        close();
    }
}

void TotpExportSettingsDialog::copyToClipboard()
{
    clipboard()->setText(m_totpUri);
    if (config()->get(Config::HideWindowOnCopy).toBool()) {
        if (config()->get(Config::MinimizeOnCopy).toBool()) {
            getMainWindow()->minimizeOrHide();
        } else if (config()->get(Config::DropToBackgroundOnCopy).toBool()) {
            getMainWindow()->lower();
            window()->lower();
        }
    }
}

void TotpExportSettingsDialog::autoClose()
{
    if (--m_secTillClose > 0) {
        m_countDown->setText(tr("Closes in %1 s").arg(m_secTillClose));
    } else {
        m_timer->stop();
        close();
    }
}

void TotpExportSettingsDialog::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_intro, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_countDown, Typography::FontRole::Caption, p.textSecondary);
}

TotpExportSettingsDialog::~TotpExportSettingsDialog() = default;
