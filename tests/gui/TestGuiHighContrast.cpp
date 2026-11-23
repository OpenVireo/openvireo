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

#include "TestGuiHighContrast.h"

#include "config-keepassx-tests.h"
#include "config-keepassx.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "gui/Application.h"
#include "gui/ApplicationSettingsWidget.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/FileDialog.h"
#include "gui/MainWindow.h"
#include "gui/MessageBox.h"
#include "gui/PasswordWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultSettingsModal.h"

#include <FluentQt/FluentQt.h>

#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>

#include <functional>

int main(int argc, char* argv[])
{
    Application app(argc, argv);
    app.setApplicationName("OpenVireo");
    app.setApplicationVersion(KEEPASSXC_VERSION);
    app.setQuitOnLastWindowClosed(false);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    app.applyTheme();
    QTEST_DISABLE_KEYPAD_NAVIGATION
    TestGuiHighContrast tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

namespace
{
    // WCAG 2: 7:1 for text (AAA), 3:1 for control borders, focus rings and other non-text shapes
    constexpr qreal TextContrast = 7.0;
    constexpr qreal NonTextContrast = 3.0;

    QString describe(const QColor& a, const QColor& b)
    {
        return QStringLiteral("%1 on %2: %3:1")
            .arg(a.name(), b.name())
            .arg(FluentTheme::contrastRatio(a, b), 0, 'f', 2);
    }

#define VERIFY_CONTRAST(fg, bg, minimum)                                                                               \
    QVERIFY2(FluentTheme::contrastRatio((fg), (bg)) >= (minimum), qPrintable(#fg " / " #bg ": " + describe(fg, bg)))
} // namespace

void TestGuiHighContrast::initTestCase()
{
    QVERIFY(Crypto::init());
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), TemporaryFile::createTempConfigFile());
    QLocale::setDefault(QLocale::c());
    Application::bootstrap();

    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
    config()->set(Config::UpdateCheckMessageShown, true);
    config()->set(Config::Security_QuickUnlock, false);
    config()->set(Config::GUI_ShowExpiredEntriesOnDatabaseUnlock, false);
    config()->set(Config::Account_StartMode, "local");
    config()->set(Config::GUI_ApplicationTheme, "light");

    m_mainWindow.reset(new MainWindow());
    m_mainWindow->setAllowScreenCapture(true);
    m_mainWindow->resize(1180, 720);
    m_mainWindow->show();
    m_mainWindow->activateWindow();
    QApplication::processEvents();

    QVERIFY(m_dbFile.copyFromFile(QDir(KEEPASSX_TEST_DATA_DIR).absoluteFilePath("NewDatabase.kdbx")));
    fileDialog()->setNextFileName(m_dbFile.fileName());
    m_mainWindow->findChild<QAction*>("actionDatabaseOpen")->trigger();
    QApplication::processEvents();

    auto* tabWidget = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    m_dbWidget = tabWidget->currentDatabaseWidget();
    auto* openWidget = m_dbWidget->findChild<QWidget*>("databaseOpenWidget");
    QVERIFY(openWidget);
    auto* editPassword = openWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(editPassword);
    editPassword->setFocus();
    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);
    QTRY_VERIFY(!m_dbWidget->isLocked());
    m_db = m_dbWidget->database();
}

void TestGuiHighContrast::cleanup()
{
    setTheme("light");
}

void TestGuiHighContrast::cleanupTestCase()
{
    m_db->markAsClean();
    MessageBox::setNextAnswer(MessageBox::No);
    m_mainWindow->findChild<QAction*>("actionDatabaseClose")->trigger();
    QApplication::processEvents();
    MessageBox::setNextAnswer(MessageBox::NoButton);
    m_dbFile.remove();
}

// Switch through the settings, as the theme combo and the View menu do
void TestGuiHighContrast::setTheme(const QString& theme)
{
    config()->set(Config::GUI_ApplicationTheme, theme);
    // Setting the same value again doesn't notify, and applyHighContrast() bypasses the config
    kpxcApp->applyTheme();
    QApplication::processEvents();
}

// The explicit "High contrast" choice follows the system's light or dark appearance, so pick a
// variant directly the way Application::applyTheme() does
void TestGuiHighContrast::applyHighContrast(bool dark)
{
    const auto colors = FluentTheme::highContrastColors(dark);
    FluentTheme::applyHighContrast(colors);
    QApplication::processEvents();
}

void TestGuiHighContrast::testFixedPaletteContrast_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light on dark") << true;
    QTest::newRow("dark on light") << false;
}

void TestGuiHighContrast::testFixedPaletteContrast()
{
    QFETCH(bool, dark);
    const auto c = FluentTheme::highContrastColors(dark);
    QCOMPARE(c.isDark(), dark);

    // Text on every surface it is drawn on
    VERIFY_CONTRAST(c.windowText, c.window, TextContrast);
    VERIFY_CONTRAST(c.buttonText, c.buttonFace, TextContrast);
    VERIFY_CONTRAST(c.highlightText, c.highlight, TextContrast);
    VERIFY_CONTRAST(c.hotlight, c.window, TextContrast);
    VERIFY_CONTRAST(c.critical, c.window, TextContrast);
    VERIFY_CONTRAST(c.caution, c.window, TextContrast);
    VERIFY_CONTRAST(c.success, c.window, TextContrast);
    // Disabled text is exempt from WCAG, but should still be readable
    VERIFY_CONTRAST(c.grayText, c.window, 4.5);
    // Selection and focus outlines
    VERIFY_CONTRAST(c.highlight, c.window, NonTextContrast);

    // The Fluent-Qt tokens and semantic palette derived from the scheme
    FluentTheme::applyHighContrast(c);
    const auto theme = fluent::FluentElement::currentTheme();
    QCOMPARE(theme, dark ? fluent::FluentElement::HighContrast : fluent::FluentElement::Light);
    const auto& t = fluent::ThemeRegistry::instance().colors(theme);
    VERIFY_CONTRAST(t.textPrimary, t.bgCanvas, TextContrast);
    VERIFY_CONTRAST(t.textSecondary, t.bgLayer, TextContrast);
    VERIFY_CONTRAST(t.textTertiary, t.bgLayer, TextContrast);
    VERIFY_CONTRAST(t.textPrimary, t.controlDefault, TextContrast);
    VERIFY_CONTRAST(t.textPrimary, t.controlSecondary, TextContrast);
    VERIFY_CONTRAST(t.textPrimary, t.controlTertiary, TextContrast);
    VERIFY_CONTRAST(t.textPrimary, t.subtleSecondary, TextContrast);
    VERIFY_CONTRAST(t.textPrimary, t.subtleTertiary, TextContrast);
    VERIFY_CONTRAST(t.textOnAccent, t.accentDefault, TextContrast);
    VERIFY_CONTRAST(t.textOnAccent, t.accentSecondary, TextContrast);
    VERIFY_CONTRAST(t.textOnAccent, t.accentTertiary, TextContrast);
    VERIFY_CONTRAST(t.textAccentPrimary, t.bgLayer, TextContrast);
    VERIFY_CONTRAST(t.strokeDefault, t.controlDefault, NonTextContrast);
    VERIFY_CONTRAST(t.strokeCard, t.bgLayer, NonTextContrast);
    VERIFY_CONTRAST(t.strokeDivider, t.bgLayer, NonTextContrast);
    VERIFY_CONTRAST(t.strokeFocusOuter, t.bgCanvas, NonTextContrast);

    // No translucent fills: everything a control is drawn with is opaque
    for (const auto& color : {t.accentDefault,
                              t.accentSecondary,
                              t.accentTertiary,
                              t.controlDefault,
                              t.controlSecondary,
                              t.controlTertiary,
                              t.subtleSecondary,
                              t.subtleTertiary,
                              t.strokeDefault,
                              t.strokeSecondary,
                              t.strokeCard,
                              t.strokeDivider,
                              t.bgCanvas,
                              t.bgLayer,
                              t.bgLayerOverlay}) {
        QCOMPARE(color.alpha(), 255);
    }

    const auto p = FluentTheme::palette();
    QVERIFY(p.highContrast);
    QCOMPARE(p.accentSubtle.alpha(), 255);
    VERIFY_CONTRAST(p.text, p.accentSubtle, TextContrast);
    VERIFY_CONTRAST(p.accentText, p.accentSubtle, TextContrast);
    VERIFY_CONTRAST(p.text, p.subtleHover, TextContrast);
    VERIFY_CONTRAST(p.text, p.subtlePressed, TextContrast);
    VERIFY_CONTRAST(p.selectionStroke, p.surface, NonTextContrast);
    VERIFY_CONTRAST(p.focus, p.surface, NonTextContrast);
    VERIFY_CONTRAST(p.critical, p.criticalBg, TextContrast);
    VERIFY_CONTRAST(p.caution, p.cautionBg, TextContrast);
    VERIFY_CONTRAST(p.success, p.successBg, TextContrast);
    VERIFY_CONTRAST(p.link, p.layer, TextContrast);
}

// Colours from the OS (Windows) keep the user's base colours, but status text falls back to the
// window text when the fixed status colour would be illegible on their window colour
void TestGuiHighContrast::testStatusColorsFallBack()
{
    FluentTheme::HighContrastColors c;
    c.window = QColor("#202020");
    c.windowText = QColor("#ffffff");
    c.highlight = QColor("#8ee3f0");
    c.highlightText = QColor("#263b50");
    c.buttonFace = QColor("#202020");
    c.buttonText = QColor("#ffffff");
    c.grayText = QColor("#a6a6a6");
    c.hotlight = QColor("#75e9fc");
    FluentTheme::fillStatusColors(c);
    VERIFY_CONTRAST(c.critical, c.window, TextContrast);
    VERIFY_CONTRAST(c.caution, c.window, TextContrast);
    VERIFY_CONTRAST(c.success, c.window, TextContrast);

    // A mid-grey window where the fixed status colours can't reach 7:1
    c.window = QColor("#5a5a5a");
    c.windowText = QColor("#000000");
    FluentTheme::fillStatusColors(c);
    QCOMPARE(c.critical, c.windowText);
}

void TestGuiHighContrast::testThemeSelection()
{
    // "High contrast" is offered in the settings and in View > Theme on every OS
    ApplicationSettingsWidget settings;
    settings.loadSettings();
    auto* combo = settings.findChild<QComboBox*>("appThemeComboBox");
    QVERIFY(combo);
    const int index = combo->findData(QStringLiteral("highcontrast"));
    QVERIFY(index >= 0);
    QCOMPARE(combo->itemText(index), QStringLiteral("High contrast"));

    auto* action = m_mainWindow->findChild<QAction*>("actionThemeHighContrast");
    QVERIFY(action);
    action->trigger();
    QApplication::processEvents();
    QCOMPARE(config()->get(Config::GUI_ApplicationTheme).toString(), QStringLiteral("highcontrast"));
    QVERIFY(action->isChecked());

    // Selecting it applies the high-contrast theme, without a restart
    QVERIFY(kpxcApp->isHighContrastTheme());
    QVERIFY(FluentTheme::isHighContrast());
    const auto expected = Application::highContrastColors();
    QCOMPARE(kpxcApp->isDarkTheme(), expected.isDark());
    QCOMPARE(QApplication::palette().color(QPalette::Active, QPalette::Window), expected.window);
    QCOMPARE(fluent::MotionPolicy::instance().mode(), fluent::MotionPolicy::Mode::Disabled);

    m_mainWindow->findChild<QAction*>("actionThemeLight")->trigger();
    QApplication::processEvents();
    QVERIFY(!kpxcApp->isHighContrastTheme());
    QVERIFY(!FluentTheme::isHighContrast());
    QVERIFY(QApplication::palette().color(QPalette::Active, QPalette::Window) != expected.window);
}

void TestGuiHighContrast::testPaletteRoles_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light on dark") << true;
    QTest::newRow("dark on light") << false;
}

void TestGuiHighContrast::testPaletteRoles()
{
    QFETCH(bool, dark);
    applyHighContrast(dark);
    const auto c = FluentTheme::highContrastColors(dark);

    // Stock widgets: the application QPalette is the scheme, Active and Inactive alike
    const auto pal = QApplication::palette();
    for (auto group : {QPalette::Active, QPalette::Inactive}) {
        QCOMPARE(pal.color(group, QPalette::Window), c.window);
        QCOMPARE(pal.color(group, QPalette::WindowText), c.windowText);
        QCOMPARE(pal.color(group, QPalette::Base), c.window);
        QCOMPARE(pal.color(group, QPalette::Text), c.windowText);
        QCOMPARE(pal.color(group, QPalette::Button), c.buttonFace);
        QCOMPARE(pal.color(group, QPalette::ButtonText), c.buttonText);
        QCOMPARE(pal.color(group, QPalette::Highlight), c.highlight);
        QCOMPARE(pal.color(group, QPalette::HighlightedText), c.highlightText);
        QCOMPARE(pal.color(group, QPalette::Link), c.hotlight);
    }
    QCOMPARE(pal.color(QPalette::Disabled, QPalette::Text), c.grayText);
    QCOMPARE(pal.color(QPalette::Disabled, QPalette::ButtonText), c.grayText);
    QCOMPARE(pal.color(QPalette::Disabled, QPalette::WindowText), c.grayText);
    QCOMPARE(pal.color(QPalette::Active, QPalette::ToolTipBase), c.window);
    QCOMPARE(pal.color(QPalette::Active, QPalette::ToolTipText), c.windowText);

    // The main window picks it up
    QCOMPARE(m_mainWindow->palette().color(QPalette::Window), c.window);

    // Hand-painted Fluent widgets: the semantic palette
    const auto p = FluentTheme::palette();
    QVERIFY(p.highContrast);
    QCOMPARE(FluentTheme::isDark(), dark);
    QCOMPARE(p.canvas, c.window);
    QCOMPARE(p.surface, c.window);
    QCOMPARE(p.layer, c.window);
    QCOMPARE(p.inset, c.window);
    QCOMPARE(p.text, c.windowText);
    QCOMPARE(p.textSecondary, c.windowText);
    QCOMPARE(p.textDisabled, c.grayText);
    QCOMPARE(p.stroke, c.windowText);
    QCOMPARE(p.strokeStrong, c.windowText);
    QCOMPARE(p.divider, c.windowText);
    QCOMPARE(p.accent, c.highlight);
    QCOMPARE(p.onAccent, c.highlightText);
    QCOMPARE(p.link, c.hotlight);
    QCOMPARE(p.selectionStroke, c.highlight);
    QCOMPARE(p.focus, c.highlight);
    QCOMPARE(p.critical, c.critical);
    QCOMPARE(p.criticalBg, c.window);

    // Modals hide the window rather than dimming it, and draw a solid border
    const auto modal = VaultModal::colors();
    QCOMPARE(modal.backdrop.alpha(), 255);
    QCOMPARE(modal.border, c.windowText);
    QCOMPARE(modal.selected, c.highlight);
    QCOMPARE(modal.onSelected, c.highlightText);

    QCOMPARE(fluent::MotionPolicy::instance().mode(), fluent::MotionPolicy::Mode::Disabled);
}

void TestGuiHighContrast::testLeavingHighContrastRestoresColors()
{
    setTheme("light");
    const auto light = FluentTheme::palette();
    QVERIFY(!light.highContrast);

    // The light high-contrast scheme temporarily replaces Fluent-Qt's light palette
    applyHighContrast(false);
    QCOMPARE(FluentTheme::palette().canvas, QColor(Qt::white));

    setTheme("light");
    const auto restored = FluentTheme::palette();
    QVERIFY(!restored.highContrast);
    QCOMPARE(restored.accent, light.accent);
    QCOMPARE(restored.canvas, light.canvas);
    QCOMPARE(restored.stroke, light.stroke);
    QVERIFY(restored.accentSubtle.alpha() < 255);
    QCOMPARE(fluent::MotionPolicy::instance().mode(), fluent::MotionPolicy::Mode::Full);
}

// Set OPENVIREO_HC_SCREENSHOT_DIR to save the main window and the settings card in each theme
void TestGuiHighContrast::testScreenshots()
{
    const auto dir = qEnvironmentVariable("OPENVIREO_HC_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        QSKIP("OPENVIREO_HC_SCREENSHOT_DIR not set");
    }
    QDir().mkpath(dir);

    const QList<QPair<QString, std::function<void()>>> themes = {
        {"light", [this] { setTheme("light"); }},
        {"dark", [this] { setTheme("dark"); }},
        {"hc-dark", [this] { applyHighContrast(true); }},
        {"hc-light", [this] { applyHighContrast(false); }},
    };
    for (const auto& theme : themes) {
        theme.second();
        // Fluent-Qt refreshes a token-only change (same light/dark mode) in deferred batches
        QTest::qWait(300);
        QVERIFY(m_mainWindow->grab().save(QDir(dir).filePath(theme.first + "-main.png")));

        m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
        auto* modal = m_mainWindow->findChild<VaultSettingsModal*>("vaultSettingsModal");
        QTRY_VERIFY(modal && modal->isOpen());
        QApplication::processEvents();
        QVERIFY(m_mainWindow->grab().save(QDir(dir).filePath(theme.first + "-settings.png")));
        QTest::mouseClick(modal->findChild<QPushButton*>("vaultSettingsCancel"), Qt::LeftButton);
        QTRY_VERIFY(!modal->isOpen());
    }
}
