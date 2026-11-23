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

#ifndef OPENVIREO_TESTGUIDIALOGS_H
#define OPENVIREO_TESTGUIDIALOGS_H

#include "gui/MainWindow.h"
#include "util/TemporaryFile.h"

#include <functional>

class Database;
class DatabaseTabWidget;
class DatabaseWidget;
class Entry;
class VaultDialog;

// Tests for the dialogs shown as VaultModal cards over the main window or as Fluent windows of their own
class TestGuiDialogs : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testCards_data();
    void testCards();
    void testCardKeys();
    void testCardOverDialogIsWindow();
    void testOwnWindows_data();
    void testOwnWindows();
    void testInputDialog();
    void testProgressDialog();
    void testMessageBoxWithoutWindow();
    void testScreenshots();
    void testStockWidgetScreenshots();

private:
    struct DialogCase
    {
        QString name;
        std::function<VaultDialog*()> make;
    };
    QList<DialogCase> cardCases();
    QList<DialogCase> windowCases();

    QScopedPointer<MainWindow> m_mainWindow;
    QPointer<DatabaseTabWidget> m_tabWidget;
    QPointer<DatabaseWidget> m_dbWidget;
    QSharedPointer<Database> m_db;
    TemporaryFile m_dbFile;
    QPointer<Entry> m_entry;
};

#endif // OPENVIREO_TESTGUIDIALOGS_H
