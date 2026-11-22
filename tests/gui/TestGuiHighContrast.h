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

#ifndef OPENVIREO_TESTGUIHIGHCONTRAST_H
#define OPENVIREO_TESTGUIHIGHCONTRAST_H

#include "util/TemporaryFile.h"

#include <QObject>
#include <QPointer>
#include <QScopedPointer>
#include <QSharedPointer>

class Database;
class DatabaseWidget;
#include "gui/MainWindow.h"

class TestGuiHighContrast : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void cleanupTestCase();

    void testFixedPaletteContrast_data();
    void testFixedPaletteContrast();
    void testStatusColorsFallBack();
    void testThemeSelection();
    void testPaletteRoles_data();
    void testPaletteRoles();
    void testLeavingHighContrastRestoresColors();
    void testScreenshots();

private:
    void applyHighContrast(bool dark);
    void setTheme(const QString& theme);

    QScopedPointer<MainWindow> m_mainWindow;
    QPointer<DatabaseWidget> m_dbWidget;
    QSharedPointer<Database> m_db;
    TemporaryFile m_dbFile;
};

#endif // OPENVIREO_TESTGUIHIGHCONTRAST_H
