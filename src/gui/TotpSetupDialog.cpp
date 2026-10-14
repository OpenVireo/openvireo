/*
 *  Copyright (C) 2019 KeePassXC Team <team@keepassxc.org>
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

#include "TotpSetupDialog.h"
#include "ui_TotpSetupDialog.h"

#include "core/Base32.h"
#include "core/Totp.h"
#include "gui/MessageBox.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultSettingsPage.h"

#include <components/status_info/InfoBar.h>

#include <QEvent>
#include <QScrollArea>
#include <QVBoxLayout>

namespace
{
    /**
     * Scrolls the cards once the modal is shorter than they are, rather than squeezing them. It asks for the cards'
     * full height, so a modal that fits its content grows with them, such as when the custom settings show.
     */
    class CardsScrollArea : public QScrollArea
    {
    public:
        CardsScrollArea(QWidget* cards, QWidget* parent)
            : QScrollArea(parent)
        {
            setFrameShape(QFrame::NoFrame);
            setWidgetResizable(true);
            setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            setWidget(cards);
            cards->installEventFilter(this);
            fit();
        }

        QSize sizeHint() const override
        {
            return {QScrollArea::sizeHint().width(), cardsHeight() + 2 * frameWidth()};
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == widget() && event->type() == QEvent::LayoutRequest) {
                fit();
            }
            return QScrollArea::eventFilter(watched, event);
        }

        void resizeEvent(QResizeEvent* event) override
        {
            QScrollArea::resizeEvent(event);
            // Text on the cards wraps, so their height depends on the width
            fit();
        }

    private:
        int cardsHeight() const
        {
            const int height = widget()->heightForWidth(viewport()->width());
            return height > 0 ? height : widget()->sizeHint().height();
        }

        void fit()
        {
            const int height = cardsHeight();
            if (widget()->minimumHeight() != height) {
                widget()->setMinimumHeight(height);
                updateGeometry();
            }
        }
    };
} // namespace

TotpSetupDialog::TotpSetupDialog(QWidget* parent, Entry* entry)
    : QDialog(parent)
    , m_ui(new Ui::TotpSetupDialog())
    , m_entry(entry)
{
    m_ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    if (FluentTheme::vaultLayoutEnabled()) {
        setupVaultLayout();
    } else {
        setFixedSize(sizeHint());
    }

    connect(m_ui->buttonBox, SIGNAL(rejected()), SLOT(close()));
    connect(m_ui->buttonBox, SIGNAL(accepted()), SLOT(saveSettings()));
    connect(m_ui->radioCustom, SIGNAL(toggled(bool)), SLOT(toggleCustom(bool)));

    init();
}

TotpSetupDialog::~TotpSetupDialog() = default;

// In the Vault Index layout, the dialog is the content of a modal over the window, which provides the title and the
// buttons. The key, the kind of code and the custom settings read as Fluent settings cards, and the custom settings
// only show while they apply.
void TotpSetupDialog::setupVaultLayout()
{
    auto ui = m_ui.data();
    VaultSettingsPage page(this, {});

    m_errorBar = new fluent::status_info::InfoBar(this);
    m_errorBar->setObjectName("totpErrorBar");
    m_errorBar->setSeverity(fluent::status_info::InfoBar::Error);
    m_errorBar->setIsClosable(false);
    m_errorBar->setIsOpen(true);
    m_errorBar->setSizePolicy(QSizePolicy::Expanding, m_errorBar->sizePolicy().verticalPolicy());
    m_errorBar->hide();
    page.addWidget(m_errorBar);
    ui->invalidKeyLabel->hide();
    connect(ui->seedEdit, &QLineEdit::textEdited, m_errorBar, &QWidget::hide);

    auto key = page.addSection(tr("Secret key"));
    ui->seedEdit->setPlaceholderText(tr("Base32 key, such as JBSWY3DPEHPK3PXP"));
    key->addWidget(ui->seedEdit);

    auto type = page.addSection(tr("Code type"));
    auto choices = new QWidget(this);
    auto choicesLayout = new QVBoxLayout(choices);
    choicesLayout->setContentsMargins(0, 0, 0, 0);
    choicesLayout->setSpacing(8);
    for (auto radio : {ui->radioDefault, ui->radioSteam, ui->radioCustom}) {
        choicesLayout->addWidget(radio);
    }
    // Picking custom settings shows them below, so the choice needs no colon leading into them
    ui->radioCustom->setText(tr("Custom settings"));
    type->addWidget(choices);

    auto custom = page.addSection(ui->customSettingsGroup);
    custom->addSetting(ui->algorithmLabel, ui->algorithmComboBox);
    custom->addSetting(ui->stepLabel, ui->stepSpinBox);
    custom->addSetting(ui->digitsLabel, ui->digitsSpinBox);
    ui->customSettingsGroup->setVisible(ui->radioCustom->isChecked());

    // The modal's footer replaces the dialog's buttons
    ui->buttonBox->hide();
    page.finish();

    auto cards = findChild<QWidget*>(objectName() + QStringLiteral("Cards"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(cards);
    layout()->removeWidget(cards);
    layout()->addWidget(new CardsScrollArea(cards, this));
}

void TotpSetupDialog::showError(const QString& message)
{
    m_errorBar->setTitle(tr("Invalid TOTP Secret"));
    m_errorBar->setMessage(message);
    m_errorBar->show();
}

void TotpSetupDialog::saveSettings()
{
    // Secret key sanity check
    // Convert user input to all uppercase and remove '='
    auto key = m_ui->seedEdit->text().toUpper().remove(" ").remove("=").trimmed().toLatin1();
    auto sanitizedKey = Base32::sanitizeInput(key);
    // Use startsWith to ignore added '=' for padding at the end
    if (!sanitizedKey.startsWith(key)) {
        if (m_errorBar) {
            showError(tr("The secret key must be in Base32 format, such as JBSWY3DPEHPK3PXP."));
            m_ui->seedEdit->setFocus();
            return;
        }
        MessageBox::information(this,
                                tr("Invalid TOTP Secret"),
                                tr("You have entered an invalid secret key. The key must be in Base32 format.\n"
                                   "Example: JBSWY3DPEHPK3PXP"));
        return;
    }

    QString encShortName;
    uint digits = Totp::DEFAULT_DIGITS;
    uint step = Totp::DEFAULT_STEP;
    Totp::Algorithm algorithm = Totp::DEFAULT_ALGORITHM;
    Totp::StorageFormat format = Totp::DEFAULT_FORMAT;

    if (m_ui->radioSteam->isChecked()) {
        digits = Totp::STEAM_DIGITS;
        encShortName = Totp::STEAM_SHORTNAME;
    } else if (m_ui->radioCustom->isChecked()) {
        algorithm = static_cast<Totp::Algorithm>(m_ui->algorithmComboBox->currentData().toInt());
        step = m_ui->stepSpinBox->value();
        digits = m_ui->digitsSpinBox->value();
    }

    auto settings = m_entry->totpSettings();
    if (settings) {
        if (key.isEmpty()) {
            auto answer = MessageBox::question(this,
                                               tr("Confirm Remove TOTP Settings"),
                                               tr("Are you sure you want to delete TOTP settings for this entry?"),
                                               MessageBox::Delete | MessageBox::Cancel);
            if (answer != MessageBox::Delete) {
                return;
            }
        }

        format = settings->format;
        if (format == Totp::StorageFormat::LEGACY && m_ui->radioCustom->isChecked()) {
            // Implicitly upgrade to the OTPURL format to allow for custom settings
            format = Totp::DEFAULT_FORMAT;
        }
    }

    m_entry->setTotp(Totp::createSettings(key, digits, step, format, encShortName, algorithm));
    emit totpUpdated();
    close();
}

void TotpSetupDialog::toggleCustom(bool status)
{
    m_ui->customSettingsGroup->setEnabled(status);
    if (m_errorBar) {
        m_ui->customSettingsGroup->setVisible(status);
    }
}

void TotpSetupDialog::init()
{
    // Add algorithm choices
    auto algorithms = Totp::supportedAlgorithms();
    for (const auto& item : algorithms) {
        m_ui->algorithmComboBox->addItem(item.first, item.second);
    }
    m_ui->algorithmComboBox->setCurrentIndex(0);
    m_ui->invalidKeyLabel->setVisible(false);

    // Read entry totp settings
    auto settings = m_entry->totpSettings();
    if (settings) {
        auto key = settings->key;
        m_ui->seedEdit->setText(key.remove("="));
        m_ui->seedEdit->setCursorPosition(0);
        m_ui->stepSpinBox->setValue(settings->step);

        if (settings->encoder.shortName == Totp::STEAM_SHORTNAME) {
            m_ui->radioSteam->setChecked(true);
        } else if (Totp::hasCustomSettings(settings)) {
            m_ui->radioCustom->setChecked(true);
            m_ui->digitsSpinBox->setValue(settings->digits);
            int index = m_ui->algorithmComboBox->findData(settings->algorithm);
            if (index != -1) {
                m_ui->algorithmComboBox->setCurrentIndex(index);
            }
        }

        auto error = Totp::checkValidSettings(settings);
        if (m_errorBar) {
            if (!error.isEmpty()) {
                showError(error);
            }
        } else {
            m_ui->invalidKeyLabel->setVisible(!error.isEmpty());
        }
    }
}
