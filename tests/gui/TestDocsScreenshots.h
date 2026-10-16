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

#ifndef KEEPASSXC_TESTDOCSSCREENSHOTS_H
#define KEEPASSXC_TESTDOCSSCREENSHOTS_H

#include "gui/MainWindow.h"

#include <QDir>
#include <QPointer>
#include <QScopedPointer>
#include <QTemporaryDir>

class Database;
class DatabaseTabWidget;
class DatabaseWidget;
class Entry;
class Group;
class VaultModal;

// Regenerates the documentation screenshots in docs/images from the Vault Index layout.
// Each slot drives the app into the state a docs image shows and saves it under that image's
// name. Run through the `docs_screenshots` target, or set KPXC_DOCS_SCREENSHOT_DIR.
class TestDocsScreenshots : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void mainWindow();
    void welcomeAndUnlock();
    void newDatabase();
    void entryEditor();
    void groupEditor();
    void applicationSettings();
    void databaseSettings();
    void passwordGenerator();
    void totpSetup();
    void importWizard();
    void themeComparison();

private:
    struct Callout
    {
        QString label;
        QRect rect; // in main window coordinates
        bool box = false; // outline the area as well as labelling it
    };

    void populate();
    void selectGroup(Group* group);
    void selectEntry(Entry* entry);
    bool selectPage(QWidget* root, const QString& name);
    void reveal(QWidget* widget);
    VaultModal* openModal(const char* action, const QString& name);
    void closeModal(VaultModal* modal);
    QRect rectOf(QWidget* widget) const;
    QRect controlRect(QWidget* root, const QString& name) const;
    QRect navRect(VaultModal* modal, const QString& page) const;
    QRect cardRect(VaultModal* modal) const;

    void save(const QString& name, const QList<Callout>& callouts = {}, QRect area = {});
    void saveCard(const QString& name, VaultModal* modal, const QList<Callout>& callouts = {});
    QImage capture(QRect area);

    QDir m_outputDir;
    QStringList m_saved;
    QScopedPointer<MainWindow> m_mainWindow;
    QPointer<DatabaseTabWidget> m_tabWidget;
    QPointer<DatabaseWidget> m_dbWidget;
    QSharedPointer<Database> m_db;
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QPointer<Group> m_workGroup;
    QPointer<Entry> m_github;
};

#endif // KEEPASSXC_TESTDOCSSCREENSHOTS_H
