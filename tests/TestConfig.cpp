/*
 *  Copyright (C) 2020 KeePassXC Team <team@keepassxc.org>
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

#include "TestConfig.h"

#include <QSettings>
#include <QTest>

#include "config-keepassx-tests.h"
#include "util/TemporaryFile.h"

QTEST_GUILESS_MAIN(TestConfig)

const QString oldTrueConfigPath = QString(KEEPASSX_TEST_DATA_DIR).append("/OutdatedConfig.ini");

void TestConfig::initTestCase()
{
    QLocale::setDefault(QLocale::c());
}

// upgrade config file with deprecated settings (all of which are set to non-default values)
void TestConfig::testUpgrade()
{
    TemporaryFile tempFile;

    QVERIFY(tempFile.copyFromFile(oldTrueConfigPath));
    Config::createConfigFromFile(tempFile.fileName());

    // value of new setting should be opposite the value of deprecated setting
    QVERIFY(!config()->get(Config::Security_PasswordsHidden).toBool());
    QVERIFY(config()->get(Config::Security_PasswordEmptyPlaceholder).toBool());

    tempFile.remove();
}

void TestConfig::testURLDoubleClickMigration()
{
    // Test migration from OpenURLOnDoubleClick to URLDoubleClickAction
    TemporaryFile tempFile;
    tempFile.open();

    // Create a config with old setting = true (open browser)
    QSettings oldConfig(tempFile.fileName(), QSettings::IniFormat);
    oldConfig.setValue("OpenURLOnDoubleClick", true);
    oldConfig.sync();
    tempFile.close();

    Config::createConfigFromFile(tempFile.fileName());

    // Should migrate to URLDoubleClickAction = 0 (open browser)
    QCOMPARE(config()->get(Config::URLDoubleClickAction).toInt(), 0);

    tempFile.remove();

    // Test migration from OpenURLOnDoubleClick = false (edit entry)
    TemporaryFile tempFile2;
    tempFile2.open();

    QSettings oldConfig2(tempFile2.fileName(), QSettings::IniFormat);
    oldConfig2.setValue("OpenURLOnDoubleClick", false);
    oldConfig2.sync();
    tempFile2.close();

    Config::createConfigFromFile(tempFile2.fileName());

    // Should migrate to URLDoubleClickAction = 2 (edit entry)
    QCOMPARE(config()->get(Config::URLDoubleClickAction).toInt(), 2);

    tempFile2.remove();
}

void TestConfig::testClassicLayoutMigration()
{
    // A config from before the classic theme and layout were removed
    TemporaryFile tempFile;
    tempFile.open();

    QSettings oldConfig(tempFile.fileName(), QSettings::IniFormat);
    oldConfig.setValue("ConfigVersion", 3);
    oldConfig.setValue("GUI/ApplicationTheme", "classic");
    oldConfig.setValue("GUI/HideToolbar", true);
    oldConfig.setValue("GUI/MovableToolbar", true);
    oldConfig.setValue("GUI/ToolButtonStyle", Qt::ToolButtonTextOnly);
    oldConfig.setValue("GUI/HidePreviewPanel", true);
    oldConfig.sync();
    tempFile.close();

    Config::createConfigFromFile(tempFile.fileName());

    // The classic theme follows the system now
    QCOMPARE(config()->get(Config::GUI_ApplicationTheme).toString(), QString("auto"));

    // Settings only the classic layout read are gone
    QSettings migrated(tempFile.fileName(), QSettings::IniFormat);
    QCOMPARE(migrated.value("ConfigVersion").toInt(), 4);
    QCOMPARE(migrated.value("GUI/ApplicationTheme").toString(), QString("auto"));
    for (const auto& key : {"GUI/HideToolbar", "GUI/MovableToolbar", "GUI/ToolButtonStyle", "GUI/HidePreviewPanel"}) {
        QVERIFY2(!migrated.contains(key), key);
    }

    tempFile.remove();
}
