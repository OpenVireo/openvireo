/*
 *  Copyright (C) 2023 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2012 Felix Geyer <debfx@fobos.de>
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

#include "ApplicationSettingsWidget.h"
#include "gui/osutils/OSUtilsBase.h"
#include "ui_ApplicationSettingsWidgetAutoType.h"
#include "ui_ApplicationSettingsWidgetGeneral.h"
#include "ui_ApplicationSettingsWidgetSecurity.h"
#include <QDesktopServices>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QToolTip>

#include "config-keepassx.h"

#include "autotype/AutoType.h"
#include "core/Translator.h"
#include "gui/GuiTools.h"
#include "gui/Icons.h"
#include "gui/MainWindow.h"
#include "gui/osutils/OSUtils.h"
#include "gui/styles/FluentTheme.h"
#include "gui/styles/StateColorPalette.h"
#include "gui/vault/VaultSettingsPage.h"
#include "quickunlock/QuickUnlockInterface.h"

#include "FileDialog.h"
#include "MessageBox.h"
#ifdef KPXC_FEATURE_BROWSER
#include "browser/BrowserSettingsPage.h"
#endif
#ifdef KPXC_FEATURE_AUTOFILL
#include "autofill/AutofillSettingsPage.h"
#endif
#ifdef KPXC_FEATURE_UPDATES
#include "networking/UpdateFeed.h"
#endif

class ApplicationSettingsWidget::ExtraPage
{
public:
    ExtraPage(ISettingsPage* page, QWidget* widget)
        : settingsPage(page)
        , widget(widget)
    {
    }

    void loadSettings() const
    {
        settingsPage->loadSettings(widget);
    }

    void saveSettings() const
    {
        settingsPage->saveSettings(widget);
    }

private:
    QSharedPointer<ISettingsPage> settingsPage;
    QWidget* widget;
};

ApplicationSettingsWidget::ApplicationSettingsWidget(QWidget* parent)
    : EditWidget(parent)
    , m_secWidget(new QWidget())
    , m_generalWidget(new QWidget())
    , m_autoTypeWidget(new QWidget())
    , m_secUi(new Ui::ApplicationSettingsWidgetSecurity())
    , m_generalUi(new Ui::ApplicationSettingsWidgetGeneral())
    , m_autoTypeUi(new Ui::ApplicationSettingsWidgetAutoType())
    , m_globalAutoTypeKey(static_cast<Qt::Key>(0))
    , m_globalAutoTypeModifiers(Qt::NoModifier)
{
    setHeadline(tr("Application Settings"));
    showApplyButton(false);

    m_secUi->setupUi(m_secWidget);
    m_generalUi->setupUi(m_generalWidget);
    m_autoTypeUi->setupUi(m_autoTypeWidget);
    addPage(tr("General"), icons()->icon("preferences-other"), m_generalWidget);
    addPage(tr("Security"), icons()->icon("security-high"), m_secWidget);
    if (autoType()->isAvailable()) {
        addPage(tr("Auto-Type"), icons()->icon("auto-type"), m_autoTypeWidget);
    } else {
        m_autoTypeWidget->setParent(this);
        m_autoTypeWidget->hide();
    }
#ifdef KPXC_FEATURE_BROWSER
    addSettingsPage(new BrowserSettingsPage());
#endif
#ifdef KPXC_FEATURE_AUTOFILL
    addSettingsPage(new AutofillSettingsPage());
#endif

    const bool showDesktopPortalsPreference =
#if defined(WITH_X11) && defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        !nixUtils()->isWayland();
#else
        false;
#endif
    m_autoTypeUi->autoTypePreferDesktopPortalsCheckBox->setVisible(showDesktopPortalsPreference);

    if (autoType()->isAvailable()) {
        const auto hasWindowAccess = autoType()->hasWindowAccess();
        const auto usesDesktopPortal = autoType()->usesDesktopPortal();
        if (hasWindowAccess) {
            m_autoTypeUi->autoTypeEntryTitleMatchCheckBox->setVisible(true);
            m_autoTypeUi->autoTypeEntryURLMatchCheckBox->setVisible(true);
            m_autoTypeUi->autoTypeAskCheckBox->setDisabled(false);
        } else {
            m_autoTypeUi->autoTypeEntryTitleMatchCheckBox->setVisible(false);
            m_autoTypeUi->autoTypeEntryURLMatchCheckBox->setVisible(false);
            m_autoTypeUi->autoTypeAskCheckBox->setChecked(true);
            m_autoTypeUi->autoTypeAskCheckBox->setDisabled(true);
        }

        m_autoTypeUi->autoTypeDesktopPortalPersistConnectionCheckBox->setVisible(usesDesktopPortal);
        m_autoTypeUi->autoTypeDesktopPortalUseClipboardCheckBox->setVisible(usesDesktopPortal
                                                                           && osUtils->isClipboardAvailable());
        m_autoTypeUi->autoTypeDesktopPortalPersistModeLabel->setVisible(usesDesktopPortal);
        m_autoTypeUi->autoTypeDesktopPortalPersistModeComboBox->setVisible(usesDesktopPortal);

        if (osUtils->externalGlobalShortcutsConfigurator()) {
            m_autoTypeUi->autoTypeShortcutWidget->setVisible(false);
            m_autoTypeUi->autoTypeShortcutConfigureWidget->setVisible(true);

            connect(osUtils,
                    &OSUtilsBase::globalShortcutChanged,
                    this,
                    [this](const QString& name, const QString& description) {
                        if (name != QLatin1String("autotype")) {
                            return;
                        }
                        m_autoTypeUi->autoTypeShortcutConfigureLabel->setText(description.isEmpty() ? tr("None")
                                                                                                   : description);
                    });
            connect(m_autoTypeUi->autoTypeShortcutConfigureButton, &QPushButton::clicked, osUtils, [] {
                osUtils->configureGlobalShortcut(QStringLiteral("autotype"));
            });
        } else {
            m_autoTypeUi->autoTypeShortcutWidget->setVisible(true);
            m_autoTypeUi->autoTypeShortcutConfigureWidget->setVisible(false);
        }
    }

    connect(this, SIGNAL(accepted()), SLOT(saveSettings()));
    connect(this, SIGNAL(rejected()), SLOT(reject()));

    // clang-format off
    connect(m_generalUi->autoSaveAfterEveryChangeCheckBox, SIGNAL(toggled(bool)), SLOT(autoSaveToggled(bool)));
    connect(m_generalUi->hideWindowOnCopyCheckBox, SIGNAL(toggled(bool)), SLOT(hideWindowOnCopyCheckBoxToggled(bool)));
    connect(m_generalUi->systrayShowCheckBox, SIGNAL(toggled(bool)), SLOT(systrayToggled(bool)));
    connect(m_generalUi->rememberLastDatabasesCheckBox, SIGNAL(toggled(bool)), SLOT(rememberDatabasesToggled(bool)));
    connect(m_generalUi->resetSettingsButton, SIGNAL(clicked()), SLOT(resetSettings()));
    connect(m_generalUi->importSettingsButton, SIGNAL(clicked()), SLOT(importSettings()));
    connect(m_generalUi->exportSettingsButton, SIGNAL(clicked()), SLOT(exportSettings()));
    connect(m_generalUi->useAlternativeSaveCheckBox, SIGNAL(toggled(bool)),
            m_generalUi->alternativeSaveComboBox, SLOT(setEnabled(bool)));

    connect(m_generalUi->backupBeforeSaveCheckBox, SIGNAL(toggled(bool)),
            m_generalUi->backupFilePath, SLOT(setEnabled(bool)));
    connect(m_generalUi->backupBeforeSaveCheckBox, SIGNAL(toggled(bool)),
            m_generalUi->backupFilePathPicker, SLOT(setEnabled(bool)));
    connect(m_generalUi->backupFilePathPicker, SIGNAL(pressed()), SLOT(selectBackupDirectory()));
    connect(m_generalUi->showExpiredEntriesOnDatabaseUnlockCheckBox, SIGNAL(toggled(bool)),
            SLOT(showExpiredEntriesOnDatabaseUnlockToggled(bool)));
    connect(m_autoTypeUi->autoTypeAskCheckBox, SIGNAL(toggled(bool)),
            SLOT(autoTypeAskToggled(bool)));

    connect(m_secUi->clearClipboardCheckBox, SIGNAL(toggled(bool)),
            m_secUi->clearClipboardSpinBox, SLOT(setEnabled(bool)));
    connect(m_secUi->clearSearchCheckBox, SIGNAL(toggled(bool)),
            m_secUi->clearSearchSpinBox, SLOT(setEnabled(bool)));
    connect(m_secUi->lockDatabaseIdleCheckBox, SIGNAL(toggled(bool)),
            m_secUi->lockDatabaseIdleSpinBox, SLOT(setEnabled(bool)));
    // clang-format on

    connect(m_generalUi->minimizeAfterUnlockCheckBox, &QCheckBox::toggled, this, [this](bool state) {
        if (state) {
            m_secUi->lockDatabaseMinimizeCheckBox->setChecked(false);
        }
        m_secUi->lockDatabaseMinimizeCheckBox->setToolTip(
            state ? tr("This setting cannot be enabled when minimize on unlock is enabled.") : "");
        m_secUi->lockDatabaseMinimizeCheckBox->setEnabled(!state);
    });

    // Set Auto-Type shortcut when changed
    connect(
        m_autoTypeUi->autoTypeShortcutWidget, &ShortcutWidget::shortcutChanged, this, [this](auto key, auto modifiers) {
            QString error;
            if (autoType()->registerGlobalShortcut(key, modifiers, &error)) {
                m_autoTypeUi->autoTypeShortcutWidget->setStyleSheet("");
            } else {
                QToolTip::showText(mapToGlobal(rect().bottomLeft()), error);
                StateColorPalette statePalette;
                auto color = statePalette.color(StateColorPalette::ColorRole::Error);
                m_autoTypeUi->autoTypeShortcutWidget->setStyleSheet(
                    QString("QLineEdit { background: %1; }").arg(color.name()));
            }
        });
    connect(m_autoTypeUi->autoTypeShortcutWidget, &ShortcutWidget::shortcutReset, this, [this] {
        autoType()->unregisterGlobalShortcut();
        m_autoTypeUi->autoTypeShortcutWidget->setStyleSheet("");
    });

    // Disable mouse wheel grab when scrolling
    // This prevents combo box and spinner values from changing without explicit focus
    auto mouseWheelFilter = new MouseWheelEventFilter(this);
    m_generalUi->faviconTimeoutSpinBox->installEventFilter(mouseWheelFilter);
    m_generalUi->alternativeSaveComboBox->installEventFilter(mouseWheelFilter);
    m_generalUi->toolButtonStyleComboBox->installEventFilter(mouseWheelFilter);
    m_generalUi->languageComboBox->installEventFilter(mouseWheelFilter);
    m_generalUi->trayIconAppearance->installEventFilter(mouseWheelFilter);
    m_generalUi->fontSizeComboBox->installEventFilter(mouseWheelFilter);
    m_generalUi->appThemeComboBox->installEventFilter(mouseWheelFilter);

#ifdef Q_OS_MACOS
    // The menubar is always shown on macOS, so hide the option to avoid confusion
    m_generalUi->menubarShowCheckBox->setVisible(false);
#endif

    // The Vault Index layout has no toolbar, so none of the toolbar options apply
    if (FluentTheme::vaultLayoutEnabled()) {
        m_generalUi->toolbarShowCheckBox->setVisible(false);
        m_generalUi->toolButtonStyleLabel->setVisible(false);
        m_generalUi->toolButtonStyleComboBox->setVisible(false);
        m_generalUi->toolbarMovableCheckBox->setVisible(false);
        buildVaultGeneralPage();
        buildVaultSecurityPage();
        buildVaultAutoTypePage();
    }

#ifdef KPXC_FEATURE_UPDATES
    m_generalUi->updateChannelComboBox->addItem(tr("Stable releases"), UpdateFeed::StableChannel);
    m_generalUi->updateChannelComboBox->addItem(tr("Nightly builds (untested)"), UpdateFeed::NightlyChannel);
    m_generalUi->updateChannelComboBox->installEventFilter(mouseWheelFilter);
    // Builds outside every update channel, such as local snapshots, never update
    if (!UpdateFeed::isEnabled())
#endif
    {
        m_generalUi->checkForUpdatesOnStartupCheckBox->setVisible(false);
        m_generalUi->updateChannelLabel->setVisible(false);
        m_generalUi->updateChannelComboBox->setVisible(false);
        m_generalUi->checkUpdatesSpacer->changeSize(0, 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

#ifndef KPXC_FEATURE_NETWORK
    m_secUi->privacy->setVisible(false);
    m_generalUi->faviconTimeoutLabel->setVisible(false);
    m_generalUi->faviconTimeoutSpinBox->setVisible(false);
#endif
}

ApplicationSettingsWidget::~ApplicationSettingsWidget() = default;

void ApplicationSettingsWidget::buildVaultGeneralPage()
{
    auto ui = m_generalUi.data();
    VaultSettingsPage page(m_generalWidget, tr("General"));

    auto startup = page.addSection(ui->startupGroup);
    startup->addToggle(ui->singleInstanceCheckBox);
    startup->addToggle(ui->launchAtStartup);
    startup->addToggle(ui->systrayMinimizeOnStartup);
    startup->addToggle(ui->minimizeAfterUnlockCheckBox);
    auto recent = startup->addToggle(ui->rememberLastDatabasesCheckBox);
    recent->addHeaderControl(ui->rememberLastDatabasesSpinbox);
    recent->addToggle(ui->openPreviousDatabasesOnStartupCheckBox);
    recent->addToggle(ui->rememberLastKeyFilesCheckBox);
    startup->addToggle(ui->checkForUpdatesOnStartupCheckBox);
    startup->addSetting(ui->updateChannelLabel, ui->updateChannelComboBox);
    auto expiring = startup->addToggle(ui->showExpiredEntriesOnDatabaseUnlockCheckBox);
    expiring->addHeaderControl(ui->showExpiredEntriesOnDatabaseUnlockOffsetSpinBox);

    auto files = page.addSection(ui->saveGroup);
    files->addToggle(ui->autoSaveAfterEveryChangeCheckBox);
    files->addToggle(ui->autoSaveOnExitCheckBox);
    files->addToggle(ui->autoSaveNonDataChangesCheckBox);
    files->addToggle(ui->autoReloadOnChangeCheckBox);
    auto backup = files->addToggle(ui->backupBeforeSaveCheckBox);
    backup->addSetting(ui->label, {ui->backupFilePath, ui->backupFilePathPicker});
    auto alternativeSave = files->addToggle(ui->useAlternativeSaveCheckBox);
    alternativeSave->addSetting(tr("Saving method"), ui->alternativeSaveComboBox);

    auto entries = page.addSection(ui->entryGroup);
    entries->addToggle(ui->autocompleteSuggestionsCheckBox);
    entries->addToggle(ui->ConfirmMoveEntryToRecycleBinCheckBox);
    entries->addToggle(ui->autoGeneratePasswordForNewEntriesCheckBox);
    entries->addToggle(ui->EnableCopyOnDoubleClickCheckBox);
    entries->addSetting(ui->urlDoubleClickLabel, ui->urlDoubleClickComboBox);
    entries->addToggle(ui->useGroupIconOnEntryCreationCheckBox);
    entries->addToggle(ui->minimizeOnOpenUrlCheckBox);
    auto hideOnCopy = entries->addToggle(ui->hideWindowOnCopyCheckBox);
    hideOnCopy->addWidget(ui->minimizeOnCopyRadioButton);
    hideOnCopy->addWidget(ui->dropToBackgroundOnCopyRadioButton);
    entries->addSetting(ui->faviconTimeoutLabel, ui->faviconTimeoutSpinBox);

    auto appearance = page.addSection(ui->generalGroup);
    appearance->addSetting(ui->languageLabel_2, ui->languageComboBox, tr("Takes effect after restarting OpenVireo"));
    appearance->addSetting(ui->appThemeLabel, ui->appThemeComboBox);
    appearance->addSetting(ui->fontSizeLabel, ui->fontSizeComboBox);
    appearance->addToggle(ui->menubarShowCheckBox);
    appearance->addToggle(ui->colorPasswordsCheckBox);
    appearance->addToggle(ui->monospaceNotesCheckBox);
    appearance->addToggle(ui->minimizeOnCloseCheckBox);
    auto tray = appearance->addToggle(ui->systrayShowCheckBox);
    tray->addSetting(ui->trayIconAppearanceLabel, ui->trayIconAppearance);
    tray->addToggle(ui->systrayMinimizeToTrayCheckBox);

    auto manage = page.addSection(tr("Manage Settings"));
    manage->addSetting(tr("Move your settings to another computer"),
                       {ui->importSettingsButton, ui->exportSettingsButton});
    manage->addSetting(tr("Return every setting to its default value"), ui->resetSettingsButton);

    page.finish();
}

void ApplicationSettingsWidget::buildVaultSecurityPage()
{
    auto ui = m_secUi.data();
    VaultSettingsPage page(m_secWidget, tr("Security"));

    auto timeouts = page.addSection(ui->groupBox);
    timeouts->addToggle(ui->clearClipboardCheckBox)->addHeaderControl(ui->clearClipboardSpinBox);
    timeouts->addToggle(ui->clearSearchCheckBox)->addHeaderControl(ui->clearSearchSpinBox);
    timeouts->addToggle(ui->lockDatabaseIdleCheckBox)->addHeaderControl(ui->lockDatabaseIdleSpinBox);

    auto locking = page.addSection(ui->lockOptionsGroup);
    locking->addToggle(ui->quickUnlockCheckBox);
    locking->addToggle(ui->lockDatabaseOnScreenLockCheckBox);
    locking->addToggle(ui->lockDatabasesOnUserSwitchCheckBox);
    locking->addToggle(ui->lockDatabaseMinimizeCheckBox);

    auto convenience = page.addSection(ui->groupBox_2);
    convenience->addToggle(ui->passwordShowDotsCheckBox);
    convenience->addToggle(ui->passwordsHiddenCheckBox);
    convenience->addToggle(ui->passwordPreviewCleartextCheckBox);
    convenience->addToggle(ui->hideTotpCheckBox);
    convenience->addToggle(ui->hideNotesCheckBox);

    auto privacy = page.addSection(ui->privacy);
    privacy->addToggle(ui->fallbackToSearch);

    page.finish();
}

void ApplicationSettingsWidget::buildVaultAutoTypePage()
{
    auto ui = m_autoTypeUi.data();
    VaultSettingsPage page(m_autoTypeWidget, tr("Auto-Type"));

    // Only one of the shortcut field and the button for the system's shortcut settings is shown
    auto global = page.addSection(tr("Global Auto-Type"));
    global->addSetting(ui->autoTypeShortcutLabel, {ui->autoTypeShortcutWidget, ui->autoTypeShortcutConfigureWidget});
    global->addToggle(ui->autoTypeEntryTitleMatchCheckBox);
    global->addToggle(ui->autoTypeEntryURLMatchCheckBox);

    auto behavior = page.addSection(tr("Behavior"));
    auto ask = behavior->addToggle(ui->autoTypeAskCheckBox);
    ask->addToggle(ui->autoTypeSkipMainWindowConfirmationCheckBox);
    behavior->addToggle(ui->autoTypeHideExpiredEntryCheckBox);
    behavior->addToggle(ui->autoTypeRelockDatabaseCheckBox);
    behavior->addSetting(ui->autoTypeRetypeLabel, ui->autoTypeRetypeTimeSpinBox);
    behavior->addSetting(ui->autoTypeStartDelayLabel, ui->autoTypeStartDelaySpinBox);
    behavior->addSetting(ui->autoTypeDelayLabel, ui->autoTypeDelaySpinBox);

    // Only used with desktop portals on Wayland
    auto remote = page.addSection(tr("Remote Desktop"));
    remote->addToggle(ui->autoTypePreferDesktopPortalsCheckBox);
    remote->addToggle(ui->autoTypeDesktopPortalPersistConnectionCheckBox);
    remote->addToggle(ui->autoTypeDesktopPortalUseClipboardCheckBox);
    remote->addSetting(ui->autoTypeDesktopPortalPersistModeLabel, ui->autoTypeDesktopPortalPersistModeComboBox);

    page.finish();
}

void ApplicationSettingsWidget::addSettingsPage(ISettingsPage* page)
{
    QWidget* widget = page->createWidget();
    widget->setParent(this);
    m_extraPages.append(ExtraPage(page, widget));
    addPage(page->name(), page->icon(), widget);
}

void ApplicationSettingsWidget::loadSettings()
{
    if (config()->hasAccessError()) {
        showMessage(tr("Access error for config file %1").arg(config()->getFileName()), MessageWidget::Error);
    }

#ifdef QT_DEBUG
    m_generalUi->singleInstanceCheckBox->setEnabled(false);
    m_generalUi->launchAtStartup->setEnabled(false);
#endif
    m_generalUi->singleInstanceCheckBox->setChecked(config()->get(Config::SingleInstance).toBool());
    m_generalUi->launchAtStartup->setChecked(osUtils->isLaunchAtStartupEnabled());
    m_generalUi->rememberLastDatabasesCheckBox->setChecked(config()->get(Config::RememberLastDatabases).toBool());
    m_generalUi->rememberLastDatabasesSpinbox->setValue(config()->get(Config::NumberOfRememberedLastDatabases).toInt());
    m_generalUi->rememberLastKeyFilesCheckBox->setChecked(config()->get(Config::RememberLastKeyFiles).toBool());
    m_generalUi->openPreviousDatabasesOnStartupCheckBox->setChecked(
        config()->get(Config::OpenPreviousDatabasesOnStartup).toBool());
    m_generalUi->autoSaveAfterEveryChangeCheckBox->setChecked(config()->get(Config::AutoSaveAfterEveryChange).toBool());
    m_generalUi->autoSaveOnExitCheckBox->setChecked(config()->get(Config::AutoSaveOnExit).toBool());
    m_generalUi->autoSaveNonDataChangesCheckBox->setChecked(config()->get(Config::AutoSaveNonDataChanges).toBool());
    m_generalUi->backupBeforeSaveCheckBox->setChecked(config()->get(Config::BackupBeforeSave).toBool());

    m_generalUi->backupFilePath->setText(config()->get(Config::BackupFilePathPattern).toString());

    m_generalUi->useAlternativeSaveCheckBox->setChecked(!config()->get(Config::UseAtomicSaves).toBool());
    m_generalUi->alternativeSaveComboBox->setCurrentIndex(config()->get(Config::UseDirectWriteSaves).toBool() ? 1 : 0);
    m_generalUi->autoReloadOnChangeCheckBox->setChecked(config()->get(Config::AutoReloadOnChange).toBool());
    m_generalUi->minimizeAfterUnlockCheckBox->setChecked(config()->get(Config::MinimizeAfterUnlock).toBool());
    m_generalUi->minimizeOnOpenUrlCheckBox->setChecked(config()->get(Config::MinimizeOnOpenUrl).toBool());
    m_generalUi->urlDoubleClickComboBox->setCurrentIndex(config()->get(Config::URLDoubleClickAction).toInt());
    m_generalUi->hideWindowOnCopyCheckBox->setChecked(config()->get(Config::HideWindowOnCopy).toBool());
    hideWindowOnCopyCheckBoxToggled(m_generalUi->hideWindowOnCopyCheckBox->isChecked());
    m_generalUi->minimizeOnCopyRadioButton->setChecked(config()->get(Config::MinimizeOnCopy).toBool());
    m_generalUi->dropToBackgroundOnCopyRadioButton->setChecked(config()->get(Config::DropToBackgroundOnCopy).toBool());
    m_generalUi->useGroupIconOnEntryCreationCheckBox->setChecked(
        config()->get(Config::UseGroupIconOnEntryCreation).toBool());
    m_autoTypeUi->autoTypeEntryTitleMatchCheckBox->setChecked(config()->get(Config::AutoTypeEntryTitleMatch).toBool());
    m_autoTypeUi->autoTypeEntryURLMatchCheckBox->setChecked(config()->get(Config::AutoTypeEntryURLMatch).toBool());
    m_autoTypeUi->autoTypeHideExpiredEntryCheckBox->setChecked(config()->get(Config::AutoTypeHideExpiredEntry).toBool());
    m_autoTypeUi->autoTypePreferDesktopPortalsCheckBox->setChecked(
        config()->get(Config::AutoTypePreferDesktopPortals).toBool());
    m_autoTypeUi->autoTypeDesktopPortalPersistConnectionCheckBox->setChecked(
        config()->get(Config::AutoTypeDesktopPortalPersistConnection).toBool());
    m_autoTypeUi->autoTypeDesktopPortalUseClipboardCheckBox->setChecked(
        config()->get(Config::AutoTypeDesktopPortalUseClipboard).toBool());
    m_autoTypeUi->autoTypeDesktopPortalPersistModeComboBox->setCurrentIndex(
        config()->get(Config::AutoTypeDesktopPortalPersistMode).toUInt());
    m_generalUi->faviconTimeoutSpinBox->setValue(config()->get(Config::FaviconDownloadTimeout).toInt());
    m_generalUi->autocompleteSuggestionsCheckBox->setChecked(config()->get(Config::AutocompleteSuggestions).toBool());
    m_generalUi->ConfirmMoveEntryToRecycleBinCheckBox->setChecked(
        !config()->get(Config::Security_NoConfirmMoveEntryToRecycleBin).toBool());
    m_generalUi->EnableCopyOnDoubleClickCheckBox->setChecked(
        config()->get(Config::Security_EnableCopyOnDoubleClick).toBool());
    m_generalUi->autoGeneratePasswordForNewEntriesCheckBox->setChecked(
        config()->get(Config::AutoGeneratePasswordForNewEntries).toBool());

    m_generalUi->languageComboBox->clear();
    QList<QPair<QString, QString>> languages = Translator::availableLanguages();
    for (const auto& language : languages) {
        m_generalUi->languageComboBox->addItem(language.second, language.first);
    }
    int defaultIndex = m_generalUi->languageComboBox->findData(config()->get(Config::GUI_Language));
    if (defaultIndex > 0) {
        m_generalUi->languageComboBox->setCurrentIndex(defaultIndex);
    }

    m_generalUi->menubarShowCheckBox->setChecked(!config()->get(Config::GUI_HideMenubar).toBool());
    m_generalUi->toolbarShowCheckBox->setChecked(!config()->get(Config::GUI_HideToolbar).toBool());
    m_generalUi->toolbarMovableCheckBox->setChecked(config()->get(Config::GUI_MovableToolbar).toBool());
    m_generalUi->monospaceNotesCheckBox->setChecked(config()->get(Config::GUI_MonospaceNotes).toBool());
    m_generalUi->colorPasswordsCheckBox->setChecked(config()->get(Config::GUI_ColorPasswords).toBool());

    m_generalUi->toolButtonStyleComboBox->clear();
    m_generalUi->toolButtonStyleComboBox->addItem(tr("Icon only"), Qt::ToolButtonIconOnly);
    m_generalUi->toolButtonStyleComboBox->addItem(tr("Text only"), Qt::ToolButtonTextOnly);
    m_generalUi->toolButtonStyleComboBox->addItem(tr("Text beside icon"), Qt::ToolButtonTextBesideIcon);
    m_generalUi->toolButtonStyleComboBox->addItem(tr("Text under icon"), Qt::ToolButtonTextUnderIcon);
    m_generalUi->toolButtonStyleComboBox->addItem(tr("Follow style"), Qt::ToolButtonFollowStyle);
    int toolButtonStyleIndex =
        m_generalUi->toolButtonStyleComboBox->findData(config()->get(Config::GUI_ToolButtonStyle));
    if (toolButtonStyleIndex >= 0) {
        m_generalUi->toolButtonStyleComboBox->setCurrentIndex(toolButtonStyleIndex);
    }

    m_generalUi->fontSizeComboBox->clear();
    m_generalUi->fontSizeComboBox->addItem(tr("Small"), -1);
    m_generalUi->fontSizeComboBox->addItem(tr("Normal"), 0);
    m_generalUi->fontSizeComboBox->addItem(tr("Medium"), 1);
    m_generalUi->fontSizeComboBox->addItem(tr("Large"), 2);

    int fontSizeIndex = m_generalUi->fontSizeComboBox->findData(config()->get(Config::GUI_FontSizeOffset));
    if (fontSizeIndex >= 0) {
        m_generalUi->fontSizeComboBox->setCurrentIndex(fontSizeIndex);
    } else {
        // Custom value entered into config file, add it to the list and select it
        m_generalUi->fontSizeComboBox->addItem(tr("Custom"), config()->get(Config::GUI_FontSizeOffset).toInt());
        m_generalUi->fontSizeComboBox->setCurrentIndex(m_generalUi->fontSizeComboBox->count() - 1);
    }

    m_generalUi->appThemeComboBox->clear();
    m_generalUi->appThemeComboBox->addItem(tr("Automatic (follow system)"), QStringLiteral("auto"));
    m_generalUi->appThemeComboBox->addItem(tr("Light"), QStringLiteral("light"));
    m_generalUi->appThemeComboBox->addItem(tr("Dark"), QStringLiteral("dark"));
    auto appTheme = config()->get(Config::GUI_ApplicationTheme).toString();
    int appThemeIndex = m_generalUi->appThemeComboBox->findData(appTheme);
    if (appThemeIndex < 0) {
        // Keep the classic theme selectable when it is already in use
        m_generalUi->appThemeComboBox->addItem(tr("Classic"), appTheme);
        appThemeIndex = m_generalUi->appThemeComboBox->count() - 1;
    }
    m_generalUi->appThemeComboBox->setCurrentIndex(appThemeIndex);

    m_generalUi->systrayShowCheckBox->setChecked(config()->get(Config::GUI_ShowTrayIcon).toBool());
    systrayToggled(m_generalUi->systrayShowCheckBox->isChecked());
    m_generalUi->systrayMinimizeToTrayCheckBox->setChecked(config()->get(Config::GUI_MinimizeToTray).toBool());
    m_generalUi->minimizeOnCloseCheckBox->setChecked(config()->get(Config::GUI_MinimizeOnClose).toBool());
    m_generalUi->systrayMinimizeOnStartup->setChecked(config()->get(Config::GUI_MinimizeOnStartup).toBool());
    m_generalUi->checkForUpdatesOnStartupCheckBox->setChecked(config()->get(Config::GUI_CheckForUpdates).toBool());
#ifdef KPXC_FEATURE_UPDATES
    m_generalUi->updateChannelComboBox->setCurrentIndex(
        qMax(0, m_generalUi->updateChannelComboBox->findData(UpdateFeed::channel())));
#endif

    m_generalUi->showExpiredEntriesOnDatabaseUnlockCheckBox->setChecked(
        config()->get(Config::GUI_ShowExpiredEntriesOnDatabaseUnlock).toBool());
    m_generalUi->showExpiredEntriesOnDatabaseUnlockOffsetSpinBox->setValue(
        config()->get(Config::GUI_ShowExpiredEntriesOnDatabaseUnlockOffsetDays).toInt());
    showExpiredEntriesOnDatabaseUnlockToggled(m_generalUi->showExpiredEntriesOnDatabaseUnlockCheckBox->isChecked());

    m_autoTypeUi->autoTypeAskCheckBox->setChecked(config()->get(Config::Security_AutoTypeAsk).toBool());
    m_autoTypeUi->autoTypeSkipMainWindowConfirmationCheckBox->setChecked(
        config()->get(Config::Security_AutoTypeSkipMainWindowConfirmation).toBool());
    autoTypeAskToggled(m_autoTypeUi->autoTypeAskCheckBox->isChecked());
    m_autoTypeUi->autoTypeRelockDatabaseCheckBox->setChecked(config()->get(Config::Security_RelockAutoType).toBool());

    if (autoType()->isAvailable()) {
        m_globalAutoTypeKey = static_cast<Qt::Key>(config()->get(Config::GlobalAutoTypeKey).toInt());
        m_globalAutoTypeModifiers =
            static_cast<Qt::KeyboardModifiers>(config()->get(Config::GlobalAutoTypeModifiers).toInt());
        if (!osUtils->externalGlobalShortcutsConfigurator() && m_globalAutoTypeKey > 0
            && m_globalAutoTypeModifiers > 0) {
            m_autoTypeUi->autoTypeShortcutWidget->setShortcut(m_globalAutoTypeKey, m_globalAutoTypeModifiers);
        }
        m_autoTypeUi->autoTypeRetypeTimeSpinBox->setValue(config()->get(Config::GlobalAutoTypeRetypeTime).toInt());
        m_autoTypeUi->autoTypeShortcutWidget->setAttribute(Qt::WA_MacShowFocusRect, true);
        m_autoTypeUi->autoTypeDelaySpinBox->setValue(config()->get(Config::AutoTypeDelay).toInt());
        m_autoTypeUi->autoTypeStartDelaySpinBox->setValue(config()->get(Config::AutoTypeStartDelay).toInt());
    }

    m_generalUi->trayIconAppearance->clear();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    m_generalUi->trayIconAppearance->addItem(tr("Monochrome"), "monochrome");
#else
    m_generalUi->trayIconAppearance->addItem(tr("Monochrome (light)"), "monochrome-light");
    m_generalUi->trayIconAppearance->addItem(tr("Monochrome (dark)"), "monochrome-dark");
#endif
    m_generalUi->trayIconAppearance->addItem(tr("Colorful"), "colorful");
    int trayIconIndex = m_generalUi->trayIconAppearance->findData(icons()->trayIconAppearance());
    if (trayIconIndex > 0) {
        m_generalUi->trayIconAppearance->setCurrentIndex(trayIconIndex);
    }

    m_secUi->clearClipboardCheckBox->setChecked(config()->get(Config::Security_ClearClipboard).toBool());
    m_secUi->clearClipboardSpinBox->setValue(config()->get(Config::Security_ClearClipboardTimeout).toInt());

    m_secUi->clearSearchCheckBox->setChecked(config()->get(Config::Security_ClearSearch).toBool());
    m_secUi->clearSearchSpinBox->setValue(config()->get(Config::Security_ClearSearchTimeout).toInt());

    m_secUi->lockDatabaseIdleCheckBox->setChecked(config()->get(Config::Security_LockDatabaseIdle).toBool());
    m_secUi->lockDatabaseIdleSpinBox->setValue(config()->get(Config::Security_LockDatabaseIdleSeconds).toInt());
    m_secUi->lockDatabaseMinimizeCheckBox->setChecked(m_secUi->lockDatabaseMinimizeCheckBox->isEnabled()
                                                      && config()->get(Config::Security_LockDatabaseMinimize).toBool());
    m_secUi->lockDatabaseOnScreenLockCheckBox->setChecked(
        config()->get(Config::Security_LockDatabaseScreenLock).toBool());
#if defined(Q_OS_MACOS)
    m_secUi->lockDatabasesOnUserSwitchCheckBox->setVisible(true);
#else
    m_secUi->lockDatabasesOnUserSwitchCheckBox->setVisible(false);
#endif
    m_secUi->lockDatabasesOnUserSwitchCheckBox->setChecked(
        config()->get(Config::Security_LockDatabaseOnUserSwitch).toBool());
    m_secUi->fallbackToSearch->setChecked(config()->get(Config::Security_IconDownloadFallback).toBool());

    m_secUi->passwordsHiddenCheckBox->setChecked(config()->get(Config::Security_PasswordsHidden).toBool());
    m_secUi->passwordShowDotsCheckBox->setChecked(config()->get(Config::Security_PasswordEmptyPlaceholder).toBool());
    m_secUi->passwordPreviewCleartextCheckBox->setChecked(
        config()->get(Config::Security_HidePasswordPreviewPanel).toBool());
    m_secUi->hideTotpCheckBox->setChecked(config()->get(Config::Security_HideTotpPreviewPanel).toBool());
    m_secUi->hideNotesCheckBox->setChecked(config()->get(Config::Security_HideNotes).toBool());

    m_secUi->quickUnlockCheckBox->setEnabled(getQuickUnlock()->isAvailable());
    m_secUi->quickUnlockCheckBox->setChecked(config()->get(Config::Security_QuickUnlock).toBool());

    for (const ExtraPage& page : asConst(m_extraPages)) {
        page.loadSettings();
    }

    setCurrentPage(0);
}

void ApplicationSettingsWidget::saveSettings()
{
    if (config()->hasAccessError()) {
        showMessage(tr("Access error for config file %1").arg(config()->getFileName()), MessageWidget::Error);
        // We prevent closing the settings page if we could not write to
        // the config file.
        return;
    }

#ifndef QT_DEBUG
    osUtils->setLaunchAtStartup(m_generalUi->launchAtStartup->isChecked());
#endif

    config()->set(Config::SingleInstance, m_generalUi->singleInstanceCheckBox->isChecked());
    config()->set(Config::RememberLastDatabases, m_generalUi->rememberLastDatabasesCheckBox->isChecked());
    config()->set(Config::NumberOfRememberedLastDatabases, m_generalUi->rememberLastDatabasesSpinbox->value());
    config()->set(Config::RememberLastKeyFiles, m_generalUi->rememberLastKeyFilesCheckBox->isChecked());
    config()->set(Config::OpenPreviousDatabasesOnStartup,
                  m_generalUi->openPreviousDatabasesOnStartupCheckBox->isChecked());
    config()->set(Config::AutoSaveAfterEveryChange, m_generalUi->autoSaveAfterEveryChangeCheckBox->isChecked());
    config()->set(Config::AutoSaveOnExit, m_generalUi->autoSaveOnExitCheckBox->isChecked());
    config()->set(Config::AutoSaveNonDataChanges, m_generalUi->autoSaveNonDataChangesCheckBox->isChecked());
    config()->set(Config::BackupBeforeSave, m_generalUi->backupBeforeSaveCheckBox->isChecked());

    config()->set(Config::BackupFilePathPattern, m_generalUi->backupFilePath->text());

    config()->set(Config::UseAtomicSaves, !m_generalUi->useAlternativeSaveCheckBox->isChecked());
    config()->set(Config::UseDirectWriteSaves, m_generalUi->alternativeSaveComboBox->currentIndex() == 1);
    config()->set(Config::AutoReloadOnChange, m_generalUi->autoReloadOnChangeCheckBox->isChecked());
    config()->set(Config::MinimizeAfterUnlock, m_generalUi->minimizeAfterUnlockCheckBox->isChecked());
    config()->set(Config::MinimizeOnOpenUrl, m_generalUi->minimizeOnOpenUrlCheckBox->isChecked());
    config()->set(Config::URLDoubleClickAction, m_generalUi->urlDoubleClickComboBox->currentIndex());
    config()->set(Config::HideWindowOnCopy, m_generalUi->hideWindowOnCopyCheckBox->isChecked());
    config()->set(Config::MinimizeOnCopy, m_generalUi->minimizeOnCopyRadioButton->isChecked());
    config()->set(Config::DropToBackgroundOnCopy, m_generalUi->dropToBackgroundOnCopyRadioButton->isChecked());
    config()->set(Config::UseGroupIconOnEntryCreation, m_generalUi->useGroupIconOnEntryCreationCheckBox->isChecked());
    config()->set(Config::AutoTypeEntryTitleMatch, m_autoTypeUi->autoTypeEntryTitleMatchCheckBox->isChecked());
    config()->set(Config::AutoTypeEntryURLMatch, m_autoTypeUi->autoTypeEntryURLMatchCheckBox->isChecked());
    config()->set(Config::AutoTypeHideExpiredEntry, m_autoTypeUi->autoTypeHideExpiredEntryCheckBox->isChecked());
    const auto preferDesktopPortals = m_autoTypeUi->autoTypePreferDesktopPortalsCheckBox->isChecked();
    const auto preferDesktopPortalsChanged =
        config()->get(Config::AutoTypePreferDesktopPortals).toBool() != preferDesktopPortals;
    if (preferDesktopPortalsChanged) {
        getMainWindow()->displayGlobalMessage(
            tr("Restart OpenVireo to apply the Auto-Type desktop portals preference."), MessageWidget::Information);
    }
    config()->set(Config::AutoTypePreferDesktopPortals, preferDesktopPortals);
    config()->set(Config::AutoTypeDesktopPortalPersistConnection,
                  m_autoTypeUi->autoTypeDesktopPortalPersistConnectionCheckBox->isChecked());
    config()->set(Config::AutoTypeDesktopPortalUseClipboard,
                  m_autoTypeUi->autoTypeDesktopPortalUseClipboardCheckBox->isChecked());
    config()->set(Config::AutoTypeDesktopPortalPersistMode,
                  m_autoTypeUi->autoTypeDesktopPortalPersistModeComboBox->currentIndex());
    config()->set(Config::FaviconDownloadTimeout, m_generalUi->faviconTimeoutSpinBox->value());
    config()->set(Config::Security_NoConfirmMoveEntryToRecycleBin,
                  !m_generalUi->ConfirmMoveEntryToRecycleBinCheckBox->isChecked());
    config()->set(Config::Security_EnableCopyOnDoubleClick, m_generalUi->EnableCopyOnDoubleClickCheckBox->isChecked());
    config()->set(Config::AutocompleteSuggestions, m_generalUi->autocompleteSuggestionsCheckBox->isChecked());
    config()->set(Config::AutoGeneratePasswordForNewEntries,
                  m_generalUi->autoGeneratePasswordForNewEntriesCheckBox->isChecked());

    auto language = m_generalUi->languageComboBox->currentData().toString();
    if (config()->get(Config::GUI_Language) != language) {
        QTimer::singleShot(200, [] {
            getMainWindow()->restartApp(
                tr("You must restart the application to set the new language. Would you like to restart now?"));
        });
    }
    config()->set(Config::GUI_Language, language);

    config()->set(Config::GUI_HideMenubar, !m_generalUi->menubarShowCheckBox->isChecked());
    config()->set(Config::GUI_HideToolbar, !m_generalUi->toolbarShowCheckBox->isChecked());
    config()->set(Config::GUI_MovableToolbar, m_generalUi->toolbarMovableCheckBox->isChecked());
    config()->set(Config::GUI_MonospaceNotes, m_generalUi->monospaceNotesCheckBox->isChecked());
    config()->set(Config::GUI_ColorPasswords, m_generalUi->colorPasswordsCheckBox->isChecked());

    config()->set(Config::GUI_ToolButtonStyle, m_generalUi->toolButtonStyleComboBox->currentData().toString());
    config()->set(Config::GUI_FontSizeOffset, m_generalUi->fontSizeComboBox->currentData().toInt());
    config()->set(Config::GUI_ApplicationTheme, m_generalUi->appThemeComboBox->currentData().toString());

    config()->set(Config::GUI_ShowTrayIcon, m_generalUi->systrayShowCheckBox->isChecked());
    config()->set(Config::GUI_TrayIconAppearance, m_generalUi->trayIconAppearance->currentData().toString());
    config()->set(Config::GUI_MinimizeToTray, m_generalUi->systrayMinimizeToTrayCheckBox->isChecked());
    config()->set(Config::GUI_MinimizeOnClose, m_generalUi->minimizeOnCloseCheckBox->isChecked());
    config()->set(Config::GUI_MinimizeOnStartup, m_generalUi->systrayMinimizeOnStartup->isChecked());
    config()->set(Config::GUI_CheckForUpdates, m_generalUi->checkForUpdatesOnStartupCheckBox->isChecked());
#ifdef KPXC_FEATURE_UPDATES
    // Keeping the build's own channel stores nothing, so the setting follows the build if it is replaced
    auto updateChannel = m_generalUi->updateChannelComboBox->currentData().toString();
    config()->set(Config::GUI_UpdateChannel, updateChannel == UpdateFeed::buildChannel() ? QString() : updateChannel);
#endif

    config()->set(Config::GUI_ShowExpiredEntriesOnDatabaseUnlock,
                  m_generalUi->showExpiredEntriesOnDatabaseUnlockCheckBox->isChecked());
    config()->set(Config::GUI_ShowExpiredEntriesOnDatabaseUnlockOffsetDays,
                  m_generalUi->showExpiredEntriesOnDatabaseUnlockOffsetSpinBox->value());

    config()->set(Config::Security_AutoTypeAsk, m_autoTypeUi->autoTypeAskCheckBox->isChecked());
    config()->set(Config::Security_AutoTypeSkipMainWindowConfirmation,
                  m_autoTypeUi->autoTypeSkipMainWindowConfirmationCheckBox->isChecked());
    config()->set(Config::Security_RelockAutoType, m_autoTypeUi->autoTypeRelockDatabaseCheckBox->isChecked());

    if (autoType()->isAvailable()) {
        if (m_autoTypeUi->autoTypeShortcutWidget->isVisible()) {
            config()->set(Config::GlobalAutoTypeKey, m_autoTypeUi->autoTypeShortcutWidget->key());
            config()->set(Config::GlobalAutoTypeModifiers,
                          static_cast<int>(m_autoTypeUi->autoTypeShortcutWidget->modifiers()));
        }
        config()->set(Config::GlobalAutoTypeRetypeTime, m_autoTypeUi->autoTypeRetypeTimeSpinBox->value());
        config()->set(Config::AutoTypeDelay, m_autoTypeUi->autoTypeDelaySpinBox->value());
        config()->set(Config::AutoTypeStartDelay, m_autoTypeUi->autoTypeStartDelaySpinBox->value());
    }
    config()->set(Config::Security_ClearClipboard, m_secUi->clearClipboardCheckBox->isChecked());
    config()->set(Config::Security_ClearClipboardTimeout, m_secUi->clearClipboardSpinBox->value());

    config()->set(Config::Security_ClearSearch, m_secUi->clearSearchCheckBox->isChecked());
    config()->set(Config::Security_ClearSearchTimeout, m_secUi->clearSearchSpinBox->value());

    config()->set(Config::Security_LockDatabaseIdle, m_secUi->lockDatabaseIdleCheckBox->isChecked());
    config()->set(Config::Security_LockDatabaseIdleSeconds, m_secUi->lockDatabaseIdleSpinBox->value());
    config()->set(Config::Security_LockDatabaseMinimize, m_secUi->lockDatabaseMinimizeCheckBox->isChecked());
    config()->set(Config::Security_LockDatabaseScreenLock, m_secUi->lockDatabaseOnScreenLockCheckBox->isChecked());
    config()->set(Config::Security_LockDatabaseOnUserSwitch, m_secUi->lockDatabasesOnUserSwitchCheckBox->isChecked());
    config()->set(Config::Security_IconDownloadFallback, m_secUi->fallbackToSearch->isChecked());

    config()->set(Config::Security_PasswordsHidden, m_secUi->passwordsHiddenCheckBox->isChecked());
    config()->set(Config::Security_PasswordEmptyPlaceholder, m_secUi->passwordShowDotsCheckBox->isChecked());

    config()->set(Config::Security_HidePasswordPreviewPanel, m_secUi->passwordPreviewCleartextCheckBox->isChecked());
    config()->set(Config::Security_HideTotpPreviewPanel, m_secUi->hideTotpCheckBox->isChecked());
    config()->set(Config::Security_HideNotes, m_secUi->hideNotesCheckBox->isChecked());

    if (m_secUi->quickUnlockCheckBox->isEnabled()) {
        config()->set(Config::Security_QuickUnlock, m_secUi->quickUnlockCheckBox->isChecked());
    }

    // Security: clear storage if related settings are disabled
    if (!config()->get(Config::RememberLastDatabases).toBool()) {
        config()->remove(Config::LastDir);
        config()->remove(Config::LastDatabases);
        config()->remove(Config::LastActiveDatabase);
    }

    if (!config()->get(Config::RememberLastKeyFiles).toBool()) {
        config()->remove(Config::LastDir);
        config()->remove(Config::LastKeyFiles);
        config()->remove(Config::LastChallengeResponse);
    }

    for (const ExtraPage& page : asConst(m_extraPages)) {
        page.saveSettings();
    }
}

void ApplicationSettingsWidget::resetSettings()
{
    // Confirm reset
    auto ans = MessageBox::question(this,
                                    tr("Confirm Reset"),
                                    tr("Are you sure you want to reset all settings to default?"),
                                    MessageBox::Reset | MessageBox::Cancel,
                                    MessageBox::Cancel);
    if (ans == MessageBox::Cancel) {
        return;
    }

    if (config()->hasAccessError()) {
        showMessage(tr("Access error for config file %1").arg(config()->getFileName()), MessageWidget::Error);
        // We prevent closing the settings page if we could not write to
        // the config file.
        return;
    }

    // Reset general and security settings to default
    config()->resetToDefaults();

    // Clear recently used data
    config()->remove(Config::LastDatabases);
    config()->remove(Config::LastActiveDatabase);
    config()->remove(Config::LastKeyFiles);
    config()->remove(Config::LastDir);

    // Save the Extra Pages (these are NOT reset)
    for (const ExtraPage& page : asConst(m_extraPages)) {
        page.saveSettings();
    }

    config()->sync();

    // Refresh the settings widget and notify listeners
    loadSettings();
    emit settingsReset();
}

void ApplicationSettingsWidget::importSettings()
{
    auto file = fileDialog()->getOpenFileName(this, tr("Import OpenVireo Settings"), {}, "*.ini");
    if (file.isEmpty()) {
        return;
    }

    if (!config()->importSettings(file)) {
        showMessage(tr("Failed to import settings from %1, not a valid settings file.").arg(file),
                    MessageWidget::Error);
        return;
    }

    loadSettings();
    emit settingsReset();
}

void ApplicationSettingsWidget::exportSettings()
{
    auto file = fileDialog()->getSaveFileName(this, tr("Export OpenVireo Settings"), {}, "*.ini");
    if (file.isEmpty()) {
        return;
    }

    config()->exportSettings(file);
}

void ApplicationSettingsWidget::reject()
{
    // register the old key again as it might have changed
    if (m_globalAutoTypeKey > 0 && m_globalAutoTypeModifiers > 0) {
        autoType()->registerGlobalShortcut(m_globalAutoTypeKey, m_globalAutoTypeModifiers);
    }
}

void ApplicationSettingsWidget::autoSaveToggled(bool checked)
{
    // Explicitly enable other auto-save options
    if (checked) {
        m_generalUi->autoSaveOnExitCheckBox->setChecked(true);
        m_generalUi->autoSaveNonDataChangesCheckBox->setChecked(true);
    }
    m_generalUi->autoSaveOnExitCheckBox->setEnabled(!checked);
    m_generalUi->autoSaveNonDataChangesCheckBox->setEnabled(!checked);
}

void ApplicationSettingsWidget::hideWindowOnCopyCheckBoxToggled(bool checked)
{
    m_generalUi->minimizeOnCopyRadioButton->setEnabled(checked);
    m_generalUi->dropToBackgroundOnCopyRadioButton->setEnabled(checked);
}

void ApplicationSettingsWidget::systrayToggled(bool checked)
{
    m_generalUi->trayIconAppearance->setEnabled(checked);
    m_generalUi->trayIconAppearanceLabel->setEnabled(checked);
    m_generalUi->systrayMinimizeToTrayCheckBox->setEnabled(checked);
}

void ApplicationSettingsWidget::rememberDatabasesToggled(bool checked)
{
    if (!checked) {
        m_generalUi->rememberLastKeyFilesCheckBox->setChecked(false);
        m_generalUi->openPreviousDatabasesOnStartupCheckBox->setChecked(false);
    }

    m_generalUi->rememberLastDatabasesSpinbox->setEnabled(checked);
    m_generalUi->rememberLastKeyFilesCheckBox->setEnabled(checked);
    m_generalUi->openPreviousDatabasesOnStartupCheckBox->setEnabled(checked);
}


void ApplicationSettingsWidget::showExpiredEntriesOnDatabaseUnlockToggled(bool checked)
{
    m_generalUi->showExpiredEntriesOnDatabaseUnlockOffsetSpinBox->setEnabled(checked);
}

void ApplicationSettingsWidget::autoTypeAskToggled(bool checked)
{
    m_autoTypeUi->autoTypeSkipMainWindowConfirmationCheckBox->setEnabled(checked);
}

void ApplicationSettingsWidget::selectBackupDirectory()
{
    auto backupDirectory =
        FileDialog::instance()->getExistingDirectory(this, tr("Select backup storage directory"), QDir::homePath());
    if (!backupDirectory.isEmpty()) {
        m_generalUi->backupFilePath->setText(
            QDir(backupDirectory).filePath(config()->getDefault(Config::BackupFilePathPattern).toString()));
    }
}
