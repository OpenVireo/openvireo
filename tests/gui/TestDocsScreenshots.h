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

#include <functional>

class Database;
class DatabaseTabWidget;
class DatabaseWidget;
class Entry;
class Group;
class VaultModal;

// Regenerates the documentation screenshots in docs/images, and the website's product
// screenshots, from the Vault Index layout. Each slot drives the app into the state an image
// shows and saves it under that image's name. Docs images go to KPXC_DOCS_SCREENSHOT_DIR (the
// docs_screenshots target sets it to docs/images); the website* slots write to
// KPXC_SITE_SCREENSHOT_DIR. Slots whose folder isn't set are skipped.
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
    void websiteMainWindow();
    void websiteEntryEditor();
    void websiteDialogs();
    void websiteUnlock();

private:
    struct Callout
    {
        QString label;
        QRect rect; // in main window coordinates
        bool box = false; // outline the area as well as labelling it
    };
    // Captures one step of a walkthrough under the given name
    using Shot = std::function<void(VaultModal* modal, const QString& name)>;

    void populate();
    void unlock();
    void walkNewDatabaseWizard(const Shot& shot);
    void walkImportWizard(const Shot& shot);
    void walkPasswordGenerator(const Shot& shot);
    QImage themeComparisonImage();
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
    void saveSite(const QString& name, QImage image = {});
    QImage capture(QRect area);
    void writeImage(const QString& file, const QImage& image);

    QString m_docsDir;
    QString m_siteDir;
    QDir m_outputDir;
    QStringList m_saved;
    QStringList m_unchanged;
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
