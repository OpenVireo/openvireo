/*
 *  Copyright (C) 2013 Felix Geyer <debfx@fobos.de>
 *  Copyright (C) 2022 KeePassXC Team <team@keepassxc.org>
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

#include "PasswordGeneratorWidget.h"
#include "ui_PasswordGeneratorWidget.h"

#include <QCloseEvent>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QShortcut>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "core/Config.h"
#include "core/PasswordHealth.h"
#include "core/Resources.h"
#include "gui/Clipboard.h"
#include "gui/FileDialog.h"
#include "gui/Icons.h"
#include "gui/MessageBox.h"
#include "gui/styles/FluentTheme.h"
#include "gui/styles/StateColorPalette.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultSettingsPage.h"
#include "gui/vault/VaultWidgets.h"

namespace
{
    constexpr int SliderWidth = 220;
    constexpr int SeparatorWidth = 120;
    constexpr int ChipSpacing = 6;
    constexpr int SpinBoxWidth = 96;
    // Between the pivot and the first card, closer than between the sections of a settings page
    constexpr int PivotGap = 12;
} // namespace

PasswordGeneratorWidget::PasswordGeneratorWidget(QWidget* parent)
    : QWidget(parent)
    , m_passwordGenerator(new PasswordGenerator())
    , m_dicewareGenerator(new PassphraseGenerator())
    , m_ui(new Ui::PasswordGeneratorWidget())
{
    m_ui->setupUi(this);

    m_ui->buttonGenerate->setIcon(icons()->icon("refresh"));
    m_ui->buttonGenerate->setToolTip(
        tr("Regenerate password (%1)").arg(m_ui->buttonGenerate->shortcut().toString(QKeySequence::NativeText)));
    m_ui->buttonCopy->setIcon(icons()->icon("clipboard-text"));
    m_ui->buttonDeleteWordList->setIcon(icons()->icon("trash"));
    m_ui->buttonAddWordList->setIcon(icons()->icon("document-new"));
    m_ui->buttonClose->setShortcut(Qt::Key_Escape);

    // Add two shortcuts to save the form CTRL+Enter and CTRL+S
    auto shortcut = new QShortcut(Qt::CTRL + Qt::Key_Return, this);
    connect(shortcut, &QShortcut::activated, this, [this] { applyPassword(); });
    shortcut = new QShortcut(Qt::CTRL + Qt::Key_S, this);
    connect(shortcut, &QShortcut::activated, this, [this] { applyPassword(); });

    connect(m_ui->editNewPassword, SIGNAL(textChanged(QString)), SLOT(updateButtonsEnabled(QString)));
    connect(m_ui->editNewPassword, SIGNAL(textChanged(QString)), SLOT(updatePasswordStrength()));
    connect(m_ui->editNewPassword, SIGNAL(textChanged(QString)), SLOT(updatePasswordLengthLabel(QString)));
    connect(m_ui->buttonAdvancedMode, SIGNAL(toggled(bool)), SLOT(setAdvancedMode(bool)));
    connect(m_ui->buttonAddHex, SIGNAL(clicked()), SLOT(excludeHexChars()));
    connect(m_ui->editAdditionalChars, SIGNAL(textChanged(QString)), SLOT(updateGenerator()));
    connect(m_ui->editExcludedChars, SIGNAL(textChanged(QString)), SLOT(updateGenerator()));
    connect(m_ui->buttonApply, SIGNAL(clicked()), SLOT(applyPassword()));
    connect(m_ui->buttonCopy, SIGNAL(clicked()), SLOT(copyPassword()));
    connect(m_ui->buttonGenerate, SIGNAL(clicked()), SLOT(regeneratePassword()));
    connect(m_ui->buttonDeleteWordList, SIGNAL(clicked()), SLOT(removeCustomWordList()));
    connect(m_ui->buttonAddWordList, SIGNAL(clicked()), SLOT(addWordList()));
    connect(m_ui->buttonClose, SIGNAL(clicked()), SIGNAL(closed()));

    connect(m_ui->sliderLength, SIGNAL(valueChanged(int)), SLOT(passwordLengthChanged(int)));
    connect(m_ui->spinBoxLength, SIGNAL(valueChanged(int)), SLOT(passwordLengthChanged(int)));

    connect(m_ui->sliderWordCount, SIGNAL(valueChanged(int)), SLOT(passphraseLengthChanged(int)));
    connect(m_ui->spinBoxWordCount, SIGNAL(valueChanged(int)), SLOT(passphraseLengthChanged(int)));

    connect(m_ui->editWordSeparator, SIGNAL(textChanged(QString)), SLOT(updateGenerator()));
    connect(m_ui->comboBoxWordList, SIGNAL(currentIndexChanged(int)), SLOT(updateGenerator()));
    connect(m_ui->optionButtons, SIGNAL(buttonClicked(QAbstractButton*)), SLOT(updateGenerator()));
    connect(m_ui->tabWidget, SIGNAL(currentChanged(int)), SLOT(updateGenerator()));
    connect(m_ui->wordCaseComboBox, SIGNAL(currentIndexChanged(int)), SLOT(updateGenerator()));

    // set font size of password quality, characters, and entropy labels dynamically to 80% of
    // the default font size, but make it no smaller than 8pt
    QFont defaultFont;
    auto smallerSize = static_cast<int>(defaultFont.pointSize() * 0.8f);
    if (smallerSize >= 8) {
        defaultFont.setPointSize(smallerSize);
        m_ui->entropyLabel->setFont(defaultFont);
        m_ui->strengthLabel->setFont(defaultFont);
        m_ui->passwordLengthLabel->setFont(defaultFont);
    }

    if (FluentTheme::vaultLayoutEnabled()) {
        setupVaultHero();
        setupVaultPivot();
        setupVaultPages();
    }

    // set default separator to Space
    m_ui->editWordSeparator->setText(PassphraseGenerator::DefaultSeparator);

    // add passphrase generator case options
    m_ui->wordCaseComboBox->addItem(tr("lower case"), PassphraseGenerator::LOWERCASE);
    m_ui->wordCaseComboBox->addItem(tr("UPPER CASE"), PassphraseGenerator::UPPERCASE);
    m_ui->wordCaseComboBox->addItem(tr("Title Case"), PassphraseGenerator::TITLECASE);
    m_ui->wordCaseComboBox->addItem(tr("MIXED case"), PassphraseGenerator::MIXEDCASE);

    // load system-wide wordlists
    QDir path(resources()->wordlistPath(""));
    for (const auto& fileName : path.entryList(QDir::Files)) {
        m_ui->comboBoxWordList->addItem(tr("(SYSTEM)") + " " + fileName, fileName);
    }

    m_firstCustomWordlistIndex = m_ui->comboBoxWordList->count();

    // load user-provided wordlists
    path = QDir(resources()->userWordlistPath(""));
    for (const auto& fileName : path.entryList(QDir::Files)) {
        m_ui->comboBoxWordList->addItem(fileName, path.absolutePath() + QDir::separator() + fileName);
    }

    // Set color of wordlist warning
    StateColorPalette statePalette;
    auto color = statePalette.color(StateColorPalette::ColorRole::False);
    m_ui->labelWordListWarning->setStyleSheet(QString("QLabel { color: %1; }").arg(color.name()));

    loadSettings();
}

// In the Vault Index layout, the password is the main content of the generator: a card with the password in a large
// field, the buttons to regenerate and copy it, and its strength below as a meter and a line of figures.
void PasswordGeneratorWidget::setupVaultHero()
{
    auto ui = m_ui.data();
    auto hero = new QFrame(this);
    hero->setObjectName("generatorHero");
    hero->setProperty(VaultLineEdit::LargePaddingProperty, true);
    ui->editNewPassword->setLargeFont(true);

    for (auto button : {ui->buttonGenerate, ui->buttonCopy}) {
        button->setIcon({});
        button->setFluentLayout(fluent::basicinput::Button::IconOnly);
        button->setAccessibleName(button->toolTip());
    }
    ui->buttonGenerate->setIconGlyph(Typography::Icons::Refresh);
    ui->buttonCopy->setIconGlyph(Typography::Icons::Copy);
    ui->buttonCopy->setFluentStyle(fluent::basicinput::Button::Accent);

    auto stats = new QHBoxLayout();
    stats->setContentsMargins(0, 0, 0, 0);
    stats->setSpacing(16);
    stats->addWidget(ui->strengthLabel);
    stats->addStretch();
    stats->addWidget(ui->passwordLengthLabel);
    stats->addWidget(ui->entropyLabel);

    auto layout = new QGridLayout(hero);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setHorizontalSpacing(8);
    layout->setVerticalSpacing(10);
    layout->addWidget(ui->editNewPassword, 0, 0);
    layout->addWidget(ui->buttonGenerate, 0, 1, Qt::AlignVCenter);
    layout->addWidget(ui->buttonCopy, 0, 2, Qt::AlignVCenter);
    layout->addWidget(ui->entropyProgressBar, 1, 0, 1, 3);
    layout->addLayout(stats, 2, 0, 1, 3);
    layout->setColumnStretch(0, 1);

    // The buttons are as tall as the large field beside them
    ui->editNewPassword->installEventFilter(this);

    ui->entropyProgressBar->setFixedHeight(4);
    for (auto label : {ui->strengthLabel, ui->passwordLengthLabel, ui->entropyLabel}) {
        label->setMargin(0);
        label->setMinimumWidth(0);
    }

    Vault::followTheme(hero, [this, hero] {
        const auto p = FluentTheme::palette();
        const auto argb = [](const QColor& color) { return color.name(QColor::HexArgb); };
        hero->setStyleSheet(QStringLiteral("#generatorHero { background-color: %1; border: 1px solid %2;"
                                           " border-radius: 8px; }")
                                .arg(argb(p.inset), argb(p.stroke)));
        // The meter shows its track, so a weak password reads as a short bar rather than a stray line.
        // updatePasswordStrength() sets the color of the chunk.
        m_ui->entropyProgressBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: none; border-radius: 2px; background-color: %1; }"
                           "QProgressBar::chunk { background-color: %2; border-radius: 2px; }")
                .arg(argb(p.divider), argb(p.accent)));
        Vault::styleLabel(m_ui->strengthLabel, Typography::FontRole::BodyStrong, p.text);
        Vault::styleLabel(m_ui->passwordLengthLabel, Typography::FontRole::Caption, p.textSecondary);
        Vault::styleLabel(m_ui->entropyLabel, Typography::FontRole::Caption, p.textSecondary);
        updatePasswordStrength();
    });

    auto oldLayout = ui->passwordFieldLayout;
    ui->verticalLayout_2->insertWidget(ui->verticalLayout_2->indexOf(oldLayout), hero);
    ui->verticalLayout_2->removeItem(oldLayout);
    delete oldLayout;
}

// In the Vault Index layout, the password and passphrase tabs read as a Fluent pivot: plain labels with an accent
// line under the current one. The style draws a tab widget's own bar on a filled base, so a separate tab bar switches
// the tab widget, which keeps the current page for the generator's logic.
void PasswordGeneratorWidget::setupVaultPivot()
{
    auto tabs = m_ui->tabWidget;
    auto pivot = new QTabBar(this);
    pivot->setObjectName("generatorTypeSelector");
    pivot->setAccessibleName(tr("Generator type"));
    pivot->setDrawBase(false);
    pivot->setExpanding(false);
    for (int i = 0; i < tabs->count(); ++i) {
        pivot->addTab(tabs->tabText(i));
    }
    pivot->setCurrentIndex(tabs->currentIndex());
    connect(pivot, &QTabBar::currentChanged, tabs, &QTabWidget::setCurrentIndex);
    connect(tabs, &QTabWidget::currentChanged, pivot, &QTabBar::setCurrentIndex);
    tabs->tabBar()->hide();
    tabs->setDocumentMode(true);
    Vault::followTheme(pivot, [pivot] { pivot->setStyleSheet(Vault::pivotStyleSheet()); });

    m_ui->verticalLayout_2->insertWidget(m_ui->verticalLayout_2->indexOf(tabs), pivot, 0, Qt::AlignLeft);
}

// In the Vault Index layout, the options of each generator read as Fluent settings cards. The advanced options are a
// card of their own that a switch opens, in place of the Advanced button.
void PasswordGeneratorWidget::setupVaultPages()
{
    auto ui = m_ui.data();

    VaultSettingsPage password(ui->passwordWidget, {});
    auto options = password.addSection();
    options->layout()->setContentsMargins(0, PivotGap, 0, 0);
    ui->sliderLength->setMinimumWidth(SliderWidth);
    ui->spinBoxLength->setFixedWidth(SpinBoxWidth);
    options->addSetting(ui->labelLength, {ui->sliderLength, ui->spinBoxLength});

    // The character groups as a row of chips, with the advanced groups in a second row while they show
    auto types = new QWidget(this);
    types->setObjectName("characterTypes");
    auto typesLayout = new QVBoxLayout(types);
    typesLayout->setContentsMargins(0, 0, 0, 0);
    typesLayout->setSpacing(10);
    auto typesLabel = new QLabel(ui->groupBox->title(), types);
    Vault::followTheme(typesLabel, [typesLabel] {
        Vault::styleLabel(typesLabel, Typography::FontRole::Body, FluentTheme::palette().text);
    });
    typesLayout->addWidget(typesLabel);
    const QList<QList<QAbstractButton*>> chipRows{
        {ui->checkBoxUpper, ui->checkBoxLower, ui->checkBoxNumbers, ui->checkBoxSpecialChars, ui->checkBoxExtASCII},
        {ui->checkBoxBraces, ui->checkBoxPunctuation, ui->checkBoxQuotes, ui->checkBoxDashes, ui->checkBoxMath}};
    for (const auto& chips : chipRows) {
        auto row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(ChipSpacing);
        for (auto chip : chips) {
            row->addWidget(chip);
        }
        row->addStretch();
        typesLayout->addLayout(row);
    }
    options->addWidget(types);

    auto advancedToggle = new VaultCheckBox(tr("Advanced options"), this);
    advancedToggle->setObjectName("advancedModeToggle");
    advancedToggle->setChecked(ui->buttonAdvancedMode->isChecked());
    connect(advancedToggle, &QAbstractButton::toggled, ui->buttonAdvancedMode, &QAbstractButton::setChecked);
    connect(ui->buttonAdvancedMode, &QAbstractButton::toggled, advancedToggle, &QAbstractButton::setChecked);
    ui->buttonAdvancedMode->hide();
    auto advanced =
        options->addToggle(advancedToggle, tr("More character groups, and characters to add or leave out"));
    // The rows show while advanced mode is on, so there's no chevron to fold them away
    advanced->setCollapsible(false);
    advanced->addSetting(ui->label, ui->editAdditionalChars);
    advanced->addSetting(ui->labelExcludedChars, {ui->editExcludedChars, ui->buttonAddHex});
    advanced->addToggle(ui->checkBoxExcludeAlike, ui->checkBoxExcludeAlike->toolTip());
    advanced->addToggle(ui->checkBoxEnsureEvery);
    password.finish();

    VaultSettingsPage passphrase(ui->dicewareWidget, {});
    passphrase.addWidget(ui->labelWordListWarning);
    auto words = passphrase.addSection();
    words->layout()->setContentsMargins(0, PivotGap, 0, 0);
    ui->sliderWordCount->setMinimumWidth(SliderWidth);
    ui->spinBoxWordCount->setFixedWidth(SpinBoxWidth);
    words->addSetting(ui->labelWordCount, {ui->sliderWordCount, ui->spinBoxWordCount});
    words->addSetting(ui->labelWordList, {ui->comboBoxWordList, ui->buttonDeleteWordList, ui->buttonAddWordList});
    words->addSetting(ui->wordCaseLabel, ui->wordCaseComboBox);
    ui->editWordSeparator->setFixedWidth(SeparatorWidth);
    words->addSetting(ui->labelWordSeparator, ui->editWordSeparator);
    passphrase.finish();

    // The tabs are as tall as the current page, so the generator doesn't keep the room of the taller one
    auto fitToPage = [tabs = ui->tabWidget] {
        for (int i = 0; i < tabs->count(); ++i) {
            const auto policy = i == tabs->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored;
            tabs->widget(i)->setSizePolicy(policy, policy);
        }
        tabs->updateGeometry();
    };
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, fitToPage);
    fitToPage();

    // The options scroll below the password while the window is too short for them all
    auto tabs = ui->tabWidget;
    const int index = ui->verticalLayout_2->indexOf(tabs);
    ui->verticalLayout_2->removeWidget(tabs);
    m_optionsScroll = new QScrollArea(this);
    m_optionsScroll->setObjectName("generatorOptions");
    m_optionsScroll->setFrameShape(QFrame::NoFrame);
    m_optionsScroll->setWidgetResizable(true);
    m_optionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_optionsScroll->viewport()->setAutoFillBackground(false);
    m_optionsScroll->setWidget(tabs);
    tabs->setAutoFillBackground(false);
    // The modal fits its content, so follow the size of the options as cards show and hide
    tabs->installEventFilter(this);
    ui->verticalLayout_2->insertWidget(index, m_optionsScroll, 1);

    // Without the generator's own buttons, as in the modal, their row takes no room
    ui->horizontalSpacer_5->changeSize(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    ui->verticalSpacer->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);

    // The cards line up with the password card above them
    for (auto page : {ui->passwordWidget, ui->dicewareWidget}) {
        if (auto cards = page->findChild<QWidget*>(page->objectName() + "Cards")) {
            cards->layout()->setContentsMargins(0, 0, 0, 0);
        }
    }
}

PasswordGeneratorWidget::~PasswordGeneratorWidget() = default;

bool PasswordGeneratorWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_ui->editNewPassword && event->type() == QEvent::Resize) {
        const int size = m_ui->editNewPassword->height();
        m_ui->buttonGenerate->setFixedSize(size, size);
        m_ui->buttonCopy->setFixedSize(size, size);
    } else if (watched == m_ui->tabWidget && event->type() == QEvent::LayoutRequest && m_optionsScroll) {
        m_optionsScroll->updateGeometry();
    }
    return QWidget::eventFilter(watched, event);
}

void PasswordGeneratorWidget::closeEvent(QCloseEvent* event)
{
    // Emits closed signal when clicking X from title bar
    emit closed();
    QWidget::closeEvent(event);
}

PasswordGeneratorWidget* PasswordGeneratorWidget::popupGenerator(QWidget* parent)
{
    auto pwGenerator = new PasswordGeneratorWidget(parent);
    pwGenerator->setWindowModality(Qt::ApplicationModal);
    pwGenerator->setWindowFlags(Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    pwGenerator->setStandaloneMode(false);

    connect(pwGenerator, SIGNAL(closed()), pwGenerator, SLOT(deleteLater()));

    pwGenerator->show();
    pwGenerator->raise();
    pwGenerator->activateWindow();
    pwGenerator->adjustSize();

    return pwGenerator;
}

void PasswordGeneratorWidget::loadSettings()
{
    // Password config
    m_ui->checkBoxLower->setChecked(config()->get(Config::PasswordGenerator_LowerCase).toBool());
    m_ui->checkBoxUpper->setChecked(config()->get(Config::PasswordGenerator_UpperCase).toBool());
    m_ui->checkBoxNumbers->setChecked(config()->get(Config::PasswordGenerator_Numbers).toBool());
    m_ui->editAdditionalChars->setText(config()->get(Config::PasswordGenerator_AdditionalChars).toString());
    m_ui->editExcludedChars->setText(config()->get(Config::PasswordGenerator_ExcludedChars).toString());

    bool advanced = config()->get(Config::PasswordGenerator_AdvancedMode).toBool();
    if (advanced) {
        m_ui->checkBoxSpecialChars->setChecked(config()->get(Config::PasswordGenerator_Logograms).toBool());
    } else {
        m_ui->checkBoxSpecialChars->setChecked(config()->get(Config::PasswordGenerator_SpecialChars).toBool());
    }

    m_ui->checkBoxBraces->setChecked(config()->get(Config::PasswordGenerator_Braces).toBool());
    m_ui->checkBoxQuotes->setChecked(config()->get(Config::PasswordGenerator_Quotes).toBool());
    m_ui->checkBoxPunctuation->setChecked(config()->get(Config::PasswordGenerator_Punctuation).toBool());
    m_ui->checkBoxDashes->setChecked(config()->get(Config::PasswordGenerator_Dashes).toBool());
    m_ui->checkBoxMath->setChecked(config()->get(Config::PasswordGenerator_Math).toBool());

    m_ui->checkBoxExtASCII->setChecked(config()->get(Config::PasswordGenerator_EASCII).toBool());
    m_ui->checkBoxExcludeAlike->setChecked(config()->get(Config::PasswordGenerator_ExcludeAlike).toBool());
    m_ui->checkBoxEnsureEvery->setChecked(config()->get(Config::PasswordGenerator_EnsureEvery).toBool());
    m_ui->spinBoxLength->setValue(config()->get(Config::PasswordGenerator_Length).toInt());

    // Diceware config
    m_ui->spinBoxWordCount->setValue(config()->get(Config::PasswordGenerator_WordCount).toInt());
    m_ui->editWordSeparator->setText(config()->get(Config::PasswordGenerator_WordSeparator).toString());
    int i = m_ui->comboBoxWordList->findData(config()->get(Config::PasswordGenerator_WordList).toString());
    if (i > -1) {
        m_ui->comboBoxWordList->setCurrentIndex(i);
    }
    m_ui->wordCaseComboBox->setCurrentIndex(config()->get(Config::PasswordGenerator_WordCase).toInt());

    // Password or diceware?
    m_ui->tabWidget->setCurrentIndex(config()->get(Config::PasswordGenerator_Type).toInt());

    // Set advanced mode
    m_ui->buttonAdvancedMode->setChecked(advanced);
    setAdvancedMode(advanced);
    updateGenerator();
}

void PasswordGeneratorWidget::saveSettings()
{
    // Password config
    config()->set(Config::PasswordGenerator_LowerCase, m_ui->checkBoxLower->isChecked());
    config()->set(Config::PasswordGenerator_UpperCase, m_ui->checkBoxUpper->isChecked());
    config()->set(Config::PasswordGenerator_Numbers, m_ui->checkBoxNumbers->isChecked());
    config()->set(Config::PasswordGenerator_EASCII, m_ui->checkBoxExtASCII->isChecked());

    config()->set(Config::PasswordGenerator_AdvancedMode, m_ui->buttonAdvancedMode->isChecked());
    if (m_ui->buttonAdvancedMode->isChecked()) {
        config()->set(Config::PasswordGenerator_Logograms, m_ui->checkBoxSpecialChars->isChecked());
    } else {
        config()->set(Config::PasswordGenerator_SpecialChars, m_ui->checkBoxSpecialChars->isChecked());
    }
    config()->set(Config::PasswordGenerator_Braces, m_ui->checkBoxBraces->isChecked());
    config()->set(Config::PasswordGenerator_Punctuation, m_ui->checkBoxPunctuation->isChecked());
    config()->set(Config::PasswordGenerator_Quotes, m_ui->checkBoxQuotes->isChecked());
    config()->set(Config::PasswordGenerator_Dashes, m_ui->checkBoxDashes->isChecked());
    config()->set(Config::PasswordGenerator_Math, m_ui->checkBoxMath->isChecked());

    config()->set(Config::PasswordGenerator_AdditionalChars, m_ui->editAdditionalChars->text());
    config()->set(Config::PasswordGenerator_ExcludedChars, m_ui->editExcludedChars->text());
    config()->set(Config::PasswordGenerator_ExcludeAlike, m_ui->checkBoxExcludeAlike->isChecked());
    config()->set(Config::PasswordGenerator_EnsureEvery, m_ui->checkBoxEnsureEvery->isChecked());
    config()->set(Config::PasswordGenerator_Length, m_ui->spinBoxLength->value());

    // Diceware config
    config()->set(Config::PasswordGenerator_WordCount, m_ui->spinBoxWordCount->value());
    config()->set(Config::PasswordGenerator_WordSeparator, m_ui->editWordSeparator->text());
    config()->set(Config::PasswordGenerator_WordList, m_ui->comboBoxWordList->currentData());
    config()->set(Config::PasswordGenerator_WordCase, m_ui->wordCaseComboBox->currentIndex());

    // Password or diceware?
    config()->set(Config::PasswordGenerator_Type, m_ui->tabWidget->currentIndex());
}

void PasswordGeneratorWidget::setPasswordLength(int length)
{
    if (length > 0) {
        m_ui->spinBoxLength->setValue(length);
    } else {
        m_ui->spinBoxLength->setValue(config()->get(Config::PasswordGenerator_Length).toInt());
    }
}

void PasswordGeneratorWidget::setStandaloneMode(bool standalone)
{
    m_standalone = standalone;
    if (standalone) {
        m_ui->buttonApply->setVisible(false);
        setPasswordVisible(true);
    } else {
        m_ui->buttonApply->setVisible(true);
    }
}

QString PasswordGeneratorWidget::getGeneratedPassword()
{
    return m_ui->editNewPassword->text();
}

void PasswordGeneratorWidget::regeneratePassword()
{
    if (m_ui->tabWidget->currentIndex() == Password) {
        if (m_passwordGenerator->isValid()) {
            m_ui->editNewPassword->setText(m_passwordGenerator->generatePassword());
        }
    } else {
        m_ui->editNewPassword->setText(m_dicewareGenerator->generatePassphrase());
    }
}

void PasswordGeneratorWidget::updateButtonsEnabled(const QString& password)
{
    if (!m_standalone) {
        m_ui->buttonApply->setEnabled(!password.isEmpty());
    }
    m_ui->buttonCopy->setEnabled(!password.isEmpty());
}

void PasswordGeneratorWidget::updatePasswordStrength()
{
    // Calculate the password / passphrase health
    PasswordHealth passwordHealth(0);
    if (m_ui->tabWidget->currentIndex() == Diceware) {
        passwordHealth.init(m_dicewareGenerator->estimateEntropy());
    } else {
        passwordHealth = PasswordHealth(m_ui->editNewPassword->text());
    }

    // Update the entropy text labels
    m_ui->entropyLabel->setText(tr("Entropy: %1 bit").arg(QString::number(passwordHealth.entropy(), 'f', 2)));
    m_ui->entropyProgressBar->setValue(std::min(int(passwordHealth.entropy()), m_ui->entropyProgressBar->maximum()));

    // Update the visual strength meter
    QString style = m_ui->entropyProgressBar->styleSheet();
    QRegularExpression re("(QProgressBar::chunk\\s*\\{.*?background-color:)[^;]+;",
                          QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    style.replace(re, "\\1 %1;");

    StateColorPalette statePalette;
    switch (passwordHealth.quality()) {
    case PasswordHealth::Quality::Bad:
    case PasswordHealth::Quality::Poor:
        m_ui->entropyProgressBar->setStyleSheet(
            style.arg(statePalette.color(StateColorPalette::HealthCritical).name()));
        m_ui->strengthLabel->setText(tr("Password Quality: %1").arg(tr("Poor", "Password quality")));
        break;

    case PasswordHealth::Quality::Weak:
        m_ui->entropyProgressBar->setStyleSheet(style.arg(statePalette.color(StateColorPalette::HealthBad).name()));
        m_ui->strengthLabel->setText(tr("Password Quality: %1").arg(tr("Weak", "Password quality")));
        break;

    case PasswordHealth::Quality::Good:
        m_ui->entropyProgressBar->setStyleSheet(style.arg(statePalette.color(StateColorPalette::HealthOk).name()));
        m_ui->strengthLabel->setText(tr("Password Quality: %1").arg(tr("Good", "Password quality")));
        break;

    case PasswordHealth::Quality::Excellent:
        m_ui->entropyProgressBar->setStyleSheet(
            style.arg(statePalette.color(StateColorPalette::HealthExcellent).name()));
        m_ui->strengthLabel->setText(tr("Password Quality: %1").arg(tr("Excellent", "Password quality")));
        break;
    }
}

void PasswordGeneratorWidget::updatePasswordLengthLabel(const QString& password)
{
    m_ui->passwordLengthLabel->setText(tr("Characters: %1").arg(QString::number(password.length())));
}

void PasswordGeneratorWidget::applyPassword()
{
    saveSettings();
    m_passwordGenerated = true;
    emit appliedPassword(m_ui->editNewPassword->text());
    emit closed();
}

void PasswordGeneratorWidget::copyPassword()
{
    clipboard()->setText(m_ui->editNewPassword->text());
}

void PasswordGeneratorWidget::passwordLengthChanged(int length)
{
    m_ui->spinBoxLength->blockSignals(true);
    m_ui->sliderLength->blockSignals(true);

    m_ui->spinBoxLength->setValue(length);
    m_ui->sliderLength->setValue(length);

    m_ui->spinBoxLength->blockSignals(false);
    m_ui->sliderLength->blockSignals(false);

    updateGenerator();
}

void PasswordGeneratorWidget::passphraseLengthChanged(int length)
{
    m_ui->spinBoxWordCount->blockSignals(true);
    m_ui->sliderWordCount->blockSignals(true);

    m_ui->spinBoxWordCount->setValue(length);
    m_ui->sliderWordCount->setValue(length);

    m_ui->spinBoxWordCount->blockSignals(false);
    m_ui->sliderWordCount->blockSignals(false);

    updateGenerator();
}

void PasswordGeneratorWidget::setPasswordVisible(bool visible)
{
    m_ui->editNewPassword->setShowPassword(visible);
}

bool PasswordGeneratorWidget::isPasswordVisible() const
{
    return m_ui->editNewPassword->isPasswordVisible();
}

bool PasswordGeneratorWidget::isPasswordGenerated() const
{
    return m_passwordGenerated;
}

void PasswordGeneratorWidget::removeCustomWordList()
{
    if (m_ui->comboBoxWordList->currentIndex() < m_firstCustomWordlistIndex) {
        return;
    }

    auto wordlist = m_ui->comboBoxWordList->currentText();
    auto result = MessageBox::question(this,
                                       tr("Confirm Remove Wordlist"),
                                       tr("Do you really want to remove the wordlist \"%1\"?").arg(wordlist),
                                       MessageBox::Remove | MessageBox::Cancel,
                                       MessageBox::Cancel);

    if (result == MessageBox::Remove) {
        QFile file(m_ui->comboBoxWordList->currentData().toString());
        if (file.exists() && !file.remove()) {
            MessageBox::critical(this, tr("Failed to delete wordlist"), file.errorString());
        }

        m_ui->comboBoxWordList->removeItem(m_ui->comboBoxWordList->currentIndex());
        updateGenerator();
    }
}

void PasswordGeneratorWidget::addWordList()
{
    auto filter = QString("%1 (*.txt *.asc *.wordlist);;%2 (*)").arg(tr("Wordlists"), tr("All files"));
    auto filePath = fileDialog()->getOpenFileName(this, tr("Select Custom Wordlist"), "", filter);
    if (filePath.isEmpty()) {
        return;
    }

    // create directory for user-specified wordlists, if necessary
    QDir destDir(resources()->userWordlistPath(""));
    destDir.mkpath(".");

    // check if destination wordlist already exists
    QString fileName = QFileInfo(filePath).fileName();
    QString destPath = destDir.absolutePath() + QDir::separator() + fileName;
    QFile dest(destPath);
    if (dest.exists()) {
        auto response = MessageBox::warning(this,
                                            tr("Overwrite Wordlist?"),
                                            tr("Wordlist \"%1\" already exists as a custom wordlist.\n"
                                               "Do you want to overwrite it?")
                                                .arg(fileName),
                                            MessageBox::Overwrite | MessageBox::Cancel,
                                            MessageBox::Cancel);
        if (response != MessageBox::Overwrite) {
            return;
        }
        if (!dest.remove()) {
            MessageBox::critical(this, tr("Failed to delete wordlist"), dest.errorString());
            return;
        }
    }

    // copy wordlist to destination path and add corresponding item to the combo box
    QFile file(filePath);
    if (!file.copy(destPath)) {
        MessageBox::critical(this, tr("Failed to add wordlist"), file.errorString());
        return;
    }

    auto index = m_ui->comboBoxWordList->findData(destPath);
    if (index == -1) {
        m_ui->comboBoxWordList->addItem(fileName, destPath);
        index = m_ui->comboBoxWordList->count() - 1;
    }
    m_ui->comboBoxWordList->setCurrentIndex(index);

    // update the password generator
    updateGenerator();
}

void PasswordGeneratorWidget::setAdvancedMode(bool advanced)
{
    saveSettings();

    if (advanced) {
        m_ui->checkBoxSpecialChars->setText("# $ % && @ ^ ` ~");
        m_ui->checkBoxSpecialChars->setToolTip(tr("Logograms"));
        m_ui->checkBoxSpecialChars->setChecked(config()->get(Config::PasswordGenerator_Logograms).toBool());
    } else {
        m_ui->checkBoxSpecialChars->setText("/ * + && …");
        m_ui->checkBoxSpecialChars->setToolTip(tr("Special Characters"));
        m_ui->checkBoxSpecialChars->setChecked(config()->get(Config::PasswordGenerator_SpecialChars).toBool());
    }

    m_ui->advancedContainer->setVisible(advanced);
    // The Vault Index layout shows these on cards of their own, outside of the advanced container
    m_ui->editAdditionalChars->setVisible(advanced);
    m_ui->editExcludedChars->setVisible(advanced);
    m_ui->buttonAddHex->setVisible(advanced);
    m_ui->checkBoxExcludeAlike->setVisible(advanced);
    m_ui->checkBoxEnsureEvery->setVisible(advanced);
    m_ui->checkBoxBraces->setVisible(advanced);
    m_ui->checkBoxPunctuation->setVisible(advanced);
    m_ui->checkBoxQuotes->setVisible(advanced);
    m_ui->checkBoxMath->setVisible(advanced);
    m_ui->checkBoxDashes->setVisible(advanced);

    if (!m_standalone) {
        QTimer::singleShot(50, this, [this] { adjustSize(); });
    }
}

void PasswordGeneratorWidget::excludeHexChars()
{
    m_ui->editExcludedChars->setText("GHIJKLMNOPQRSTUVWXYZ");
    m_ui->checkBoxNumbers->setChecked(true);
    m_ui->checkBoxUpper->setChecked(true);

    m_ui->checkBoxLower->setChecked(false);
    m_ui->checkBoxSpecialChars->setChecked(false);
    m_ui->checkBoxExtASCII->setChecked(false);
    m_ui->checkBoxPunctuation->setChecked(false);
    m_ui->checkBoxQuotes->setChecked(false);
    m_ui->checkBoxDashes->setChecked(false);
    m_ui->checkBoxMath->setChecked(false);
    m_ui->checkBoxBraces->setChecked(false);

    updateGenerator();
}

PasswordGenerator::CharClasses PasswordGeneratorWidget::charClasses()
{
    PasswordGenerator::CharClasses classes;

    if (m_ui->checkBoxLower->isChecked()) {
        classes |= PasswordGenerator::LowerLetters;
    }

    if (m_ui->checkBoxUpper->isChecked()) {
        classes |= PasswordGenerator::UpperLetters;
    }

    if (m_ui->checkBoxNumbers->isChecked()) {
        classes |= PasswordGenerator::Numbers;
    }

    if (m_ui->checkBoxExtASCII->isChecked()) {
        classes |= PasswordGenerator::EASCII;
    }

    if (!m_ui->buttonAdvancedMode->isChecked()) {
        if (m_ui->checkBoxSpecialChars->isChecked()) {
            classes |= PasswordGenerator::SpecialCharacters;
        }
    } else {
        if (m_ui->checkBoxBraces->isChecked()) {
            classes |= PasswordGenerator::Braces;
        }

        if (m_ui->checkBoxPunctuation->isChecked()) {
            classes |= PasswordGenerator::Punctuation;
        }

        if (m_ui->checkBoxQuotes->isChecked()) {
            classes |= PasswordGenerator::Quotes;
        }

        if (m_ui->checkBoxDashes->isChecked()) {
            classes |= PasswordGenerator::Dashes;
        }

        if (m_ui->checkBoxMath->isChecked()) {
            classes |= PasswordGenerator::Math;
        }

        if (m_ui->checkBoxSpecialChars->isChecked()) {
            classes |= PasswordGenerator::Logograms;
        }
    }

    return classes;
}

PasswordGenerator::GeneratorFlags PasswordGeneratorWidget::generatorFlags()
{
    PasswordGenerator::GeneratorFlags flags;

    if (m_ui->buttonAdvancedMode->isChecked()) {
        if (m_ui->checkBoxExcludeAlike->isChecked()) {
            flags |= PasswordGenerator::ExcludeLookAlike;
        }

        if (m_ui->checkBoxEnsureEvery->isChecked()) {
            flags |= PasswordGenerator::CharFromEveryGroup;
        }
    }

    return flags;
}

void PasswordGeneratorWidget::updateGenerator()
{
    if (m_ui->tabWidget->currentIndex() == Password) {
        auto classes = charClasses();
        auto flags = generatorFlags();

        m_passwordGenerator->setLength(m_ui->spinBoxLength->value());
        if (m_ui->buttonAdvancedMode->isChecked()) {
            m_passwordGenerator->setCharClasses(classes);
            m_passwordGenerator->setCustomCharacterSet(m_ui->editAdditionalChars->text());
            m_passwordGenerator->setExcludedCharacterSet(m_ui->editExcludedChars->text());
        } else {
            m_passwordGenerator->setCharClasses(classes);
        }
        m_passwordGenerator->setFlags(flags);

        m_ui->buttonGenerate->setEnabled(m_passwordGenerator->isValid());
    } else {
        m_dicewareGenerator->setWordCase(
            static_cast<PassphraseGenerator::PassphraseWordCase>(m_ui->wordCaseComboBox->currentData().toInt()));

        m_dicewareGenerator->setWordCount(m_ui->spinBoxWordCount->value());
        auto path = m_ui->comboBoxWordList->currentData().toString();
        if (m_ui->comboBoxWordList->currentIndex() < m_firstCustomWordlistIndex) {
            path = resources()->wordlistPath(path);
            m_ui->buttonDeleteWordList->setEnabled(false);
        } else {
            m_ui->buttonDeleteWordList->setEnabled(true);
        }
        m_dicewareGenerator->setWordList(path);

        m_dicewareGenerator->setWordSeparator(m_ui->editWordSeparator->text());

        m_ui->labelWordListWarning->setVisible(!m_dicewareGenerator->isWordListValid());
        m_ui->buttonGenerate->setEnabled(true);
    }

    regeneratePassword();
}
