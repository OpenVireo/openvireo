/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
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

#ifndef KEEPASSXC_TESTGUIVAULT_H
#define KEEPASSXC_TESTGUIVAULT_H

#include "gui/MainWindow.h"
#include "util/TemporaryFile.h"

class Database;
class DatabaseTabWidget;
class DatabaseWidget;
class Entry;
class Group;
class VaultEntryEditCard;

// Tests for the Vault Index main window layout built on Fluent-Qt
class TestGuiVault : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testLayout();
    void testEntryListMode();
    void testDetailCardFollowsSelection();
    void testCommandBar();
    void testEditEntryInPlace();
    void testNewEntryModal();
    void testEditorSections();
    void testLeavingViewEndsEditing();
    void testSearchTitle();
    void testFluentSearchBox();
    void testDatabasePicker();
    void testWindowFrame();
    void testToolbarSettingsHidden();
    void testSettingsModal();
    void testPasswordGeneratorModal();
    void testDatabaseModals();
    void testGroupModals();
    void testNewDatabaseModal();
    void testMenuClickSurvivesLostHighlight();
    void testAutoTypeSettingsPage();
    void testFeatureSettingsPages();
    void testSidebarListsOnlyTags();
    void testScreenshots();
    void testViewScreenshots();

private:
    void selectGroup(Group* group);
    void selectEntry(Entry* entry);
    void populate();
    VaultEntryEditCard* entryEditCard() const;

    QScopedPointer<MainWindow> m_mainWindow;
    QPointer<DatabaseTabWidget> m_tabWidget;
    QPointer<DatabaseWidget> m_dbWidget;
    QSharedPointer<Database> m_db;
    TemporaryFile m_dbFile;
    QPointer<Group> m_workGroup;
    QPointer<Entry> m_github;
};

#endif // KEEPASSXC_TESTGUIVAULT_H
