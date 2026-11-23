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

#include "TestGuiDialogs.h"
#include "config-keepassx-tests.h"
#include "config-keepassx.h"

#include "autotype/AutoTypeSelectDialog.h"
#include "autotype/PickcharsDialog.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Group.h"
#include "core/Totp.h"
#include "crypto/Crypto.h"
#include "gui/Application.h"
#include "gui/CloneDialog.h"
#include "gui/DatabaseOpenDialog.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/FileDialog.h"
#include "gui/MergeDialog.h"
#include "gui/MessageBox.h"
#include "gui/PasswordWidget.h"
#include "gui/TotpDialog.h"
#include "gui/account/ShareVaultDialog.h"
#include "gui/account/SignUpWizard.h"
#include "gui/styles/FluentTheme.h"
#include "gui/entry/EditEntryAttachmentsDialog.h"
#include "gui/entry/PreviewEntryAttachmentsDialog.h"
#include "gui/export/ExportDialog.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultDialog.h"
#include "gui/vault/VaultInfoBar.h"
#include "gui/vault/VaultInputDialog.h"
#include "gui/vault/VaultModal.h"
#ifdef KPXC_FEATURE_NETWORK
#include "gui/IconDownloaderDialog.h"
#include "gui/UpdateCheckDialog.h"
#endif
#ifdef KPXC_FEATURE_BROWSER
#include "browser/BrowserEntrySaveDialog.h"
#include "gui/passkeys/PasskeyExportDialog.h"
#include "gui/passkeys/PasskeyImportDialog.h"
#include "gui/vault/VaultResponsive.h"
#endif
#ifdef KPXC_FEATURE_SSHAGENT
#include "sshagent/OpenSSHKeyGenDialog.h"
#endif

#include <components/dialogs_flyouts/ContentDialog.h>
#include <components/status_info/ProgressRing.h>

#include <QAction>
#include <QApplication>
#include <QCalendarWidget>
#include <QDateTimeEdit>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>

int main(int argc, char* argv[])
{
    Application app(argc, argv);
    app.setApplicationName("OpenVireo");
    app.setApplicationVersion(KEEPASSXC_VERSION);
    app.setQuitOnLastWindowClosed(false);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    app.applyTheme();
    // The window can be narrower than the wide layout on CI screens; TestGuiVault covers the others
    Vault::setLayoutForTests(Vault::Layout::Wide);
    QTEST_DISABLE_KEYPAD_NAVIGATION
    TestGuiDialogs tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

namespace
{
    // The visible push buttons in a widget, such as a card's footer or a window's command bar
    QList<QPushButton*> visibleButtons(QWidget* parent)
    {
        QList<QPushButton*> buttons;
        if (!parent) {
            return buttons;
        }
        // Only the Fluent ones: a window's button box stays shown for Enter, clipped away inside its bar
        for (auto* button : parent->findChildren<fluent::basicinput::Button*>()) {
            if (button->isVisible()) {
                buttons << button;
            }
        }
        return buttons;
    }

    QPushButton* visibleButton(QWidget* parent, const QString& text)
    {
        for (auto* button : visibleButtons(parent)) {
            if (VaultButton::withoutMnemonic(button->text()) == text) {
                return button;
            }
        }
        return nullptr;
    }

    QString modalPrefix(const VaultModal* modal)
    {
        auto name = modal->objectName();
        name.chop(QStringLiteral("Modal").size());
        return name;
    }

    QWidget* topLevelNamed(const QString& name)
    {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == name && widget->isVisible()) {
                return widget;
            }
        }
        return nullptr;
    }

    bool anyModalOpen(QWidget* window)
    {
        for (auto* modal : window->findChildren<VaultModal*>()) {
            if (modal->isOpen()) {
                return true;
            }
        }
        return false;
    }

    void finish(QPointer<VaultDialog>& dialog)
    {
        if (dialog) {
            dialog->deleteLater();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
} // namespace

namespace
{
    // Screenshot themes: light, dark, and the light-on-dark high-contrast scheme whatever the system uses
    void applyScreenshotTheme(const QString& theme)
    {
        if (theme == QLatin1String("highcontrast")) {
            const auto colors = FluentTheme::highContrastColors(true);
            FluentTheme::applyHighContrast(colors);
        } else {
            config()->set(Config::GUI_ApplicationTheme, theme);
            kpxcApp->applyTheme();
        }
        QApplication::processEvents();
    }
} // namespace

void TestGuiDialogs::initTestCase()
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

    m_mainWindow.reset(new MainWindow());
    m_mainWindow->setAllowScreenCapture(true);
    m_tabWidget = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    m_mainWindow->resize(1180, 760);
    m_mainWindow->show();
    m_mainWindow->activateWindow();
    QApplication::processEvents();

    QVERIFY(m_dbFile.copyFromFile(QDir(KEEPASSX_TEST_DATA_DIR).absoluteFilePath("NewDatabase.kdbx")));
    fileDialog()->setNextFileName(m_dbFile.fileName());
    m_mainWindow->findChild<QAction*>("actionDatabaseOpen")->trigger();
    QApplication::processEvents();

    m_dbWidget = m_tabWidget->currentDatabaseWidget();
    auto* editPassword = m_dbWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(editPassword);
    editPassword->setFocus();
    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);
    QTRY_VERIFY(!m_dbWidget->isLocked());
    m_db = m_dbWidget->database();

    m_entry = new Entry();
    m_entry->setUuid(QUuid::createUuid());
    m_entry->setTitle("GitHub");
    m_entry->setUsername("bongani");
    m_entry->setUrl("https://github.com/login");
    m_entry->setPassword("c7#Lq9!vRz2@pWm8xKe4");
    m_entry->setTotp(Totp::createSettings("JBSWY3DPEHPK3PXP"));
    m_entry->setGroup(m_db->rootGroup());
}

void TestGuiDialogs::cleanupTestCase()
{
    m_db->markAsClean();
    MessageBox::setNextAnswer(MessageBox::No);
    m_mainWindow->findChild<QAction*>("actionDatabaseClose")->trigger();
    QApplication::processEvents();
    MessageBox::setNextAnswer(MessageBox::NoButton);
    m_dbFile.remove();
}

void TestGuiDialogs::cleanup()
{
    // Nothing is left over the window for the next case
    QTRY_VERIFY(!anyModalOpen(m_mainWindow.data()));
    m_mainWindow->activateWindow();
}

QList<TestGuiDialogs::DialogCase> TestGuiDialogs::cardCases()
{
    QList<DialogCase> cases;
    cases << DialogCase{"clone", [this] { return new CloneDialog(m_dbWidget, m_db.data(), m_entry); }};
    cases << DialogCase{"merge", [this] {
                            auto source = QSharedPointer<Database>::create();
                            auto* entry = new Entry();
                            entry->setUuid(QUuid::createUuid());
                            entry->setTitle("Merged entry");
                            entry->setGroup(source->rootGroup());
                            return new MergeDialog(source, m_db, m_dbWidget);
                        }};
    cases << DialogCase{"totp", [this] { return new TotpDialog(m_dbWidget, m_entry); }};
    cases << DialogCase{"export", [this] { return new ExportDialog(m_db, m_tabWidget); }};
#ifdef KPXC_FEATURE_NETWORK
    cases << DialogCase{"updatecheck", [this] { return new UpdateCheckDialog(m_mainWindow.data()); }};
    cases << DialogCase{"icondownloader", [this] { return new IconDownloaderDialog(m_dbWidget); }};
#endif
#ifdef KPXC_FEATURE_BROWSER
    cases << DialogCase{"passkeyexport", [this] {
                            auto* dialog = new PasskeyExportDialog(m_dbWidget);
                            dialog->setEntries({m_entry});
                            return dialog;
                        }};
    cases << DialogCase{"passkeyimport", [this] {
                            auto* dialog = new PasskeyImportDialog(m_dbWidget);
                            dialog->setInfo("github.com", "bongani", m_db, false);
                            return dialog;
                        }};
#endif
#ifdef KPXC_FEATURE_SSHAGENT
    cases << DialogCase{"sshkeygen", [this] { return new OpenSSHKeyGenDialog(m_dbWidget); }};
#endif
    cases << DialogCase{"editattachment", [this] {
                            auto* dialog = new EditEntryAttachmentsDialog(m_dbWidget);
                            dialog->setAttachment({"notes.txt", "Recovery codes\n1234-5678\n8765-4321\n"});
                            return dialog;
                        }};
    cases << DialogCase{"previewattachment", [this] {
                            auto* dialog = new PreviewEntryAttachmentsDialog(m_dbWidget);
                            dialog->setAttachment({"notes.txt", "Recovery codes\n1234-5678\n8765-4321\n"});
                            return dialog;
                        }};
    cases << DialogCase{"sharevault",
                        [this] { return new ShareVaultDialog(nullptr, QStringLiteral("vault"), m_mainWindow.data()); }};
    cases << DialogCase{"signup", [this] { return new SignUpWizard(nullptr, m_mainWindow.data()); }};
    return cases;
}

QList<TestGuiDialogs::DialogCase> TestGuiDialogs::windowCases()
{
    QList<DialogCase> cases;
    cases << DialogCase{"autotypeselect", [this] {
                            auto* dialog = new AutoTypeSelectDialog();
                            dialog->setMatches({{m_entry, "{USERNAME}{TAB}{PASSWORD}{ENTER}"}}, {m_db}, {});
                            return dialog;
                        }};
    cases << DialogCase{"pickchars", [] { return new PickcharsDialog("c7#Lq9!vRz2@pWm8xKe4"); }};
#ifdef KPXC_FEATURE_BROWSER
    cases << DialogCase{"browsersave", [this] {
                            auto* dialog = new BrowserEntrySaveDialog();
                            QList<DatabaseWidget*> widgets{m_dbWidget, m_dbWidget};
                            dialog->setItems(widgets, m_dbWidget);
                            return dialog;
                        }};
#endif
    return cases;
}

void TestGuiDialogs::testCards_data()
{
    QTest::addColumn<int>("index");
    const auto cases = cardCases();
    for (int i = 0; i < cases.size(); ++i) {
        QTest::newRow(qPrintable(cases[i].name)) << i;
    }
}

void TestGuiDialogs::testCards()
{
    QFETCH(int, index);
    QPointer<VaultDialog> dialog = cardCases().at(index).make();
    dialog->open();
    QTRY_VERIFY(dialog->isVisible());

    // A card over the main window, titled like the dialog
    QVERIFY(dialog->isCard());
    QVERIFY(!dialog->isWindow());
    QPointer<VaultModal> modal = dialog->modal();
    QVERIFY(modal);
    QVERIFY(modal->isOpen());
    QCOMPARE(modal->parentWidget(), m_mainWindow.data());
    const auto prefix = modalPrefix(modal);
    auto* title = modal->findChild<QLabel*>(prefix + "Title");
    QVERIFY(title);
    QCOMPARE(title->text(), dialog->windowTitle());

    // The dialog's buttons are Fluent buttons in the card's footer
    auto* footer = modal->findChild<QWidget*>(prefix + "Footer");
    QVERIFY(footer);
    QVERIFY(footer->isVisible());
    const auto buttons = visibleButtons(footer);
    QVERIFY(!buttons.isEmpty());
    for (auto* button : buttons) {
        QVERIFY2(qobject_cast<VaultButton*>(button), qPrintable(button->text()));
    }

    // Escape closes the dialog, and the card with it
    auto* focus = QApplication::focusWidget();
    QTest::keyClick(focus && dialog->isAncestorOf(focus) ? focus : dialog.data(), Qt::Key_Escape);
    QTRY_VERIFY(!dialog || !dialog->isVisible());
    QTRY_VERIFY(!modal);
    finish(dialog);
}

void TestGuiDialogs::testCardKeys()
{
    const int before = m_db->rootGroup()->entries().size();

    // Enter presses the primary command: Clone
    QPointer<VaultDialog> dialog = new CloneDialog(m_dbWidget, m_db.data(), m_entry);
    dialog->open();
    QTRY_VERIFY(dialog->isCard() && dialog->modal()->isOpen());
    QTest::keyClick(dialog, Qt::Key_Enter);
    QTRY_VERIFY(!dialog || !dialog->isVisible());
    QCOMPARE(m_db->rootGroup()->entries().size(), before + 1);
    QVERIFY(m_db->rootGroup()->entries().last()->title().endsWith(" - Clone"));
    delete m_db->rootGroup()->entries().last();
    finish(dialog);

    // Cancel in the footer closes the card without cloning
    dialog = new CloneDialog(m_dbWidget, m_db.data(), m_entry);
    dialog->open();
    QTRY_VERIFY(dialog->isCard() && dialog->modal()->isOpen());
    auto* cancel = visibleButton(dialog->modal()->findChild<QWidget*>("vaultCloneFooter"), "Cancel");
    QVERIFY(cancel);
    QTest::mouseClick(cancel, Qt::LeftButton);
    QTRY_VERIFY(!dialog || !dialog->isVisible());
    QCOMPARE(m_db->rootGroup()->entries().size(), before);
    finish(dialog);

    // The close button of the card rejects too, and a dialog on the stack outlives its card
    {
        TotpDialog totp(m_dbWidget, m_entry);
        totp.setAttribute(Qt::WA_DeleteOnClose, false);
        QTimer::singleShot(0, this, [&totp] {
            QVERIFY(totp.isCard());
            QTest::mouseClick(totp.modal()->findChild<QPushButton*>("vaultTotpClose"), Qt::LeftButton);
        });
        QCOMPARE(totp.exec(), int(QDialog::Rejected));
        QVERIFY(!totp.isCard());
        QCOMPARE(totp.parentWidget(), m_dbWidget.data());
    }
}

void TestGuiDialogs::testCardOverDialogIsWindow()
{
    // Over another dialog a card would sit inside it, so the dialog is a Fluent window of its own instead
    QDialog host(m_mainWindow.data());
    host.resize(400, 300);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    QPointer<VaultDialog> dialog = new EditEntryAttachmentsDialog(&host);
    dialog->open();
    QTRY_VERIFY(dialog->isVisible());
    QVERIFY(!dialog->isCard());
    QVERIFY(dialog->isWindow());
    auto* title = dialog->findChild<QLabel*>("vaultDialogTitle");
    QVERIFY(title && title->isVisible());
    QVERIFY(visibleButton(dialog->findChild<QWidget*>("vaultDialogCommands"), "OK"));
    dialog->reject();
    QTRY_VERIFY(!dialog->isVisible());
    finish(dialog);
}

void TestGuiDialogs::testOwnWindows_data()
{
    QTest::addColumn<int>("index");
    const auto cases = windowCases();
    for (int i = 0; i < cases.size(); ++i) {
        QTest::newRow(qPrintable(cases[i].name)) << i;
    }
}

void TestGuiDialogs::testOwnWindows()
{
    QFETCH(int, index);
    QPointer<VaultDialog> dialog = windowCases().at(index).make();
    dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog));

    // A window of its own, laid out like a content dialog: the title, the content and the commands below
    QVERIFY(dialog->isWindow());
    QVERIFY(!dialog->isCard());
    QCOMPARE(dialog->presentation(), VaultDialog::OwnWindow);
    auto* title = dialog->findChild<QLabel*>("vaultDialogTitle");
    QVERIFY(title && title->isVisible());
    QCOMPARE(title->text(), dialog->windowTitle());
    auto* commands = dialog->findChild<QWidget*>("vaultDialogCommands");
    QVERIFY(commands && commands->isVisible());
    QVERIFY(!visibleButtons(commands).isEmpty());

    // Escape rejects it
    QTest::keyClick(dialog, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->isVisible());
    QCOMPARE(dialog->result(), int(QDialog::Rejected));
    finish(dialog);
}

void TestGuiDialogs::testInputDialog()
{
    using fluent::dialogs_flyouts::ContentDialog;

    // Over the window: a content dialog with a text field
    bool shown = false;
    QTimer::singleShot(0, this, [&] {
        ContentDialog* dialog = nullptr;
        QTRY_VERIFY(dialog = m_mainWindow->findChild<ContentDialog*>("vaultInputDialog"));
        QTRY_VERIFY(dialog->isVisible());
        auto close = qScopeGuard([&dialog] {
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        shown = true;
        QCOMPARE(dialog->title(), QString("New vault"));
        QCOMPARE(dialog->primaryButtonText(), QString("OK"));
        auto* field = dialog->findChild<QLineEdit*>("vaultInputField");
        QVERIFY(field);
        QCOMPARE(field->text(), QString("Personal"));
        field->setText("Work");
        auto* ok = visibleButton(dialog, "OK");
        QVERIFY(ok);
        QTest::mouseClick(ok, Qt::LeftButton);
    });
    bool ok = false;
    auto text = VaultInputDialog::getText(m_mainWindow.data(), "New vault", "Name of the new vault:", "Personal", &ok);
    QVERIFY(shown);
    QVERIFY(ok);
    QCOMPARE(text, QString("Work"));

    // Choices to pick from show in an editable combo box; Cancel answers nothing
    shown = false;
    QTimer::singleShot(0, this, [&] {
        ContentDialog* dialog = nullptr;
        QTRY_VERIFY(dialog = m_mainWindow->findChild<ContentDialog*>("vaultInputDialog"));
        QTRY_VERIFY(dialog->isVisible());
        shown = true;
        auto* combo = dialog->findChild<QComboBox*>("vaultInputField");
        QVERIFY(combo && combo->isEditable());
        QCOMPARE(combo->count(), 2);
        QTest::mouseClick(visibleButton(dialog, "Cancel"), Qt::LeftButton);
    });
    VaultInputDialog choices(m_dbWidget);
    choices.setWindowTitle("Save Search");
    choices.setComboBoxItems({"", "work"});
    QCOMPARE(choices.exec(), int(QDialog::Rejected));
    QVERIFY(shown);

    // Raised above other applications: a Fluent window of its own, where Enter accepts
    shown = false;
    QTimer::singleShot(0, this, [&] {
        QWidget* window = nullptr;
        QTRY_VERIFY(window = topLevelNamed("vaultInputDialog"));
        QVERIFY(qobject_cast<VaultDialog*>(window));
        QVERIFY(window->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        shown = true;
        auto* field = window->findChild<QLineEdit*>("vaultInputField");
        QVERIFY(field);
        field->setText("chrome-laptop");
        QTest::keyClick(field, Qt::Key_Enter);
    });
    VaultInputDialog request(m_dbWidget);
    request.setWindowTitle("New key association request");
    request.setOkButtonText("Save and allow access");
    request.setStaysOnTop(true);
    QCOMPARE(request.exec(), int(QDialog::Accepted));
    QVERIFY(shown);
    QCOMPARE(request.textValue(), QString("chrome-laptop"));
}

void TestGuiDialogs::testProgressDialog()
{
    using fluent::dialogs_flyouts::ContentDialog;

    // Shows once the task takes a moment, with a progress ring; Abort stops the task
    VaultProgressDialog progress(m_mainWindow.data(), "Forget settings", "Removing stored permissions…", "Abort", 4);
    progress.setMinimumDuration(0);
    progress.setValue(1);
    ContentDialog* dialog = nullptr;
    QTRY_VERIFY(dialog = m_mainWindow->findChild<ContentDialog*>("vaultProgressDialog"));
    QTRY_VERIFY(dialog->isVisible());
    auto* ring = dialog->findChild<fluent::status_info::ProgressRing*>("vaultProgressRing");
    QVERIFY(ring);
    QCOMPARE(ring->value(), 1);
    progress.setValue(2);
    QCOMPARE(ring->value(), 2);
    QVERIFY(!progress.wasCanceled());
    QTest::mouseClick(visibleButton(dialog, "Abort"), Qt::LeftButton);
    QTRY_VERIFY(progress.wasCanceled());
    progress.reset();
    QTRY_VERIFY(!m_mainWindow->findChild<ContentDialog*>("vaultProgressDialog"));

    // A quick task never shows it
    VaultProgressDialog quick(m_mainWindow.data(), "Forget settings", "Removing…", "Abort", 2);
    quick.setValue(1);
    quick.setValue(2);
    QVERIFY(!m_mainWindow->findChild<ContentDialog*>("vaultProgressDialog"));
}

void TestGuiDialogs::testMessageBoxWithoutWindow()
{
    // Without a window to show over, a question is a Fluent window of its own, never a QMessageBox
    bool shown = false;
    QTimer::singleShot(0, this, [&] {
        QWidget* window = nullptr;
        QTRY_VERIFY(window = topLevelNamed("vaultAlertWindow"));
        // Answered either way, so a failed check doesn't leave the test waiting on the window
        auto close = qScopeGuard([&window] {
            if (window->isVisible()) {
                qobject_cast<QDialog*>(window)->reject();
            }
        });
        shown = true;
        for (auto* widget : QApplication::topLevelWidgets()) {
            QVERIFY(!qobject_cast<QMessageBox*>(widget) || !widget->isVisible());
        }
        QCOMPARE(window->findChild<QLabel*>("vaultDialogTitle")->text(), QString("Unlock database?"));
        QVERIFY(window->findChild<QLabel*>("vaultAlertMessage")->text().contains("browser"));
        auto* commands = window->findChild<QWidget*>("vaultDialogCommands");
        QCOMPARE(visibleButtons(commands).size(), 2);
        QTest::mouseClick(visibleButton(commands, "Yes"), Qt::LeftButton);
    });
    auto answer = MessageBox::question(nullptr,
                                       "Unlock database?",
                                       "The browser asks for an entry.\nUnlock the database?",
                                       MessageBox::Yes | MessageBox::Cancel,
                                       MessageBox::Yes,
                                       MessageBox::Raise);
    QVERIFY(shown);
    QCOMPARE(answer, MessageBox::Yes);

    // Escape answers with the button that rejects
    shown = false;
    QTimer::singleShot(0, this, [&] {
        QWidget* window = nullptr;
        QTRY_VERIFY(window = topLevelNamed("vaultAlertWindow"));
        shown = true;
        QTest::keyClick(window, Qt::Key_Escape);
    });
    answer = MessageBox::warning(nullptr, "Warning", "Something happened.", MessageBox::Ok | MessageBox::Cancel);
    QVERIFY(shown);
    QCOMPARE(answer, MessageBox::Cancel);
}

void TestGuiDialogs::testScreenshots()
{
    const auto dir = qEnvironmentVariable("OPENVIREO_VAULT_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        QSKIP("Set OPENVIREO_VAULT_SCREENSHOT_DIR to save screenshots");
    }
    const auto path = [&dir](const QString& name, const QString& theme) {
        return QDir(dir).absoluteFilePath(QString("%1-%2.png").arg(name, theme));
    };
    const auto originalTheme = config()->get(Config::GUI_ApplicationTheme);

    for (const auto& theme : {QStringLiteral("light"), QStringLiteral("dark"), QStringLiteral("highcontrast")}) {
        applyScreenshotTheme(theme);
        QApplication::processEvents();
        QTest::qWait(200);

        for (const auto& card : cardCases()) {
            QPointer<VaultDialog> dialog = card.make();
            dialog->open();
            QTRY_VERIFY(dialog->isVisible());
            QTest::qWait(250);
            QVERIFY(m_mainWindow->grab().save(path("card-" + card.name, theme)));
            dialog->reject();
            QTRY_VERIFY(!anyModalOpen(m_mainWindow.data()));
            finish(dialog);
        }

        for (const auto& own : windowCases()) {
            QPointer<VaultDialog> dialog = own.make();
            dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            dialog->show();
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            QTest::qWait(250);
            QVERIFY(dialog->grab().save(path("window-" + own.name, theme)));
            dialog->reject();
            finish(dialog);
        }

        // The unlock prompt for browsers and Auto-Type, on a database of its own
        {
            DatabaseWidget target(m_dbFile.fileName());
            DatabaseOpenDialog unlock(m_mainWindow.data());
            unlock.setTarget(&target, m_dbFile.fileName());
            unlock.show();
            QVERIFY(QTest::qWaitForWindowExposed(&unlock));
            QTest::qWait(300);
            QVERIFY(unlock.grab().save(path("window-unlock", theme)));
            unlock.clearForms();
            unlock.hide();
        }

        // Prompts without a window, a field over the window and progress
        QTimer::singleShot(0, this, [&] {
            QWidget* window = nullptr;
            QTRY_VERIFY(window = topLevelNamed("vaultAlertWindow"));
            QTest::qWait(250);
            window->grab().save(path("window-alert", theme));
            QTest::keyClick(window, Qt::Key_Escape);
        });
        MessageBox::question(nullptr,
                             "Unlock database?",
                             "The browser asks for an entry of a locked database.\nUnlock it now?",
                             MessageBox::Yes | MessageBox::Cancel,
                             MessageBox::Yes,
                             MessageBox::Raise);

        QTimer::singleShot(0, this, [&] {
            fluent::dialogs_flyouts::ContentDialog* dialog = nullptr;
            QTRY_VERIFY(dialog = m_mainWindow->findChild<fluent::dialogs_flyouts::ContentDialog*>("vaultInputDialog"));
            QTest::qWait(400);
            m_mainWindow->grab().save(path("content-input", theme));
            dialog->reject();
        });
        VaultInputDialog::getText(m_mainWindow.data(), "Accept invitation", "Paste the link or the code:", {});

        {
            VaultProgressDialog progress(
                m_mainWindow.data(), "Forget site-specific settings", "Removing stored permissions…", "Abort", 4);
            progress.setMinimumDuration(0);
            progress.setValue(2);
            QTest::qWait(400);
            QVERIFY(m_mainWindow->grab().save(path("content-progress", theme)));
            progress.reset();
        }
        QTest::qWait(300);
    }

    config()->set(Config::GUI_ApplicationTheme, originalTheme);
    kpxcApp->applyTheme();
}

// The stock Qt widgets still drawn by the application style, for comparing style changes:
// headers and sort indicators, scroll bars, splitters, tabs, date/time fields, menus and tooltips
void TestGuiDialogs::testStockWidgetScreenshots()
{
    const auto dir = qEnvironmentVariable("OPENVIREO_VAULT_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        QSKIP("Set OPENVIREO_VAULT_SCREENSHOT_DIR to save screenshots");
    }
    const auto path = [&dir](const QString& name, const QString& theme) {
        return QDir(dir).absoluteFilePath(QString("%1-%2.png").arg(name, theme));
    };
    const auto originalTheme = config()->get(Config::GUI_ApplicationTheme);

    for (const auto& theme : {QStringLiteral("light"), QStringLiteral("dark"), QStringLiteral("highcontrast")}) {
        applyScreenshotTheme(theme);
        QTest::qWait(200);

        QWidget gallery;
        gallery.resize(760, 520);
        auto* layout = new QVBoxLayout(&gallery);
        auto* tabs = new QTabWidget(&gallery);
        layout->addWidget(tabs);

        auto* page = new QWidget(tabs);
        auto* pageLayout = new QVBoxLayout(page);
        auto* splitter = new QSplitter(Qt::Horizontal, page);
        auto* tree = new QTreeWidget(splitter);
        tree->setHeaderLabels({"Title", "Username", "Reason"});
        for (int i = 0; i < 30; ++i) {
            new QTreeWidgetItem(tree, {QString("Entry %1").arg(i), "user@example.com", "Reused password"});
        }
        tree->setSortingEnabled(true);
        tree->sortByColumn(0, Qt::AscendingOrder);
        tree->setCurrentItem(tree->topLevelItem(2));
        auto* scroll = new QScrollArea(splitter);
        auto* scrolled = new QPlainTextEdit(scroll);
        scrolled->setPlainText(QString("A long note\n").repeated(60));
        scroll->setWidget(scrolled);
        scroll->setWidgetResizable(true);
        pageLayout->addWidget(splitter, 1);

        auto* box = new QGroupBox("Group box", page);
        auto* boxLayout = new QHBoxLayout(box);
        auto* dateTime = new QDateTimeEdit(QDateTime(QDate(2026, 11, 23), QTime(9, 30)), box);
        dateTime->setCalendarPopup(true);
        boxLayout->addWidget(dateTime);
        auto* tool = new QToolButton(box);
        tool->setText("Tool");
        boxLayout->addWidget(tool);
        auto* progress = new QProgressBar(box);
        progress->setValue(60);
        boxLayout->addWidget(progress);
        auto* disabled = new QDateTimeEdit(box);
        disabled->setEnabled(false);
        boxLayout->addWidget(disabled);
        pageLayout->addWidget(box);
        tabs->addTab(page, "Stock widgets");
        tabs->addTab(new QWidget(tabs), "Second tab");

        gallery.show();
        QVERIFY(QTest::qWaitForWindowExposed(&gallery));
        QTest::qWait(250);
        QVERIFY(gallery.grab().save(path("stock-widgets", theme)));

        QMenu menu;
        menu.addAction("First action");
        menu.addAction("Checked action")->setCheckable(true);
        menu.actions().last()->setChecked(true);
        menu.addSeparator();
        menu.addMenu("Submenu")->addAction("Inner");
        menu.addAction("Disabled action")->setEnabled(false);
        menu.popup(gallery.mapToGlobal(QPoint(40, 40)));
        QTest::qWait(250);
        QVERIFY(menu.grab().save(path("stock-menu", theme)));
        menu.hide();

        QToolTip::showText(gallery.mapToGlobal(QPoint(60, 60)), "A tooltip on a stock widget", &gallery);
        QTest::qWait(400);
        for (auto* top : QApplication::topLevelWidgets()) {
            if (top->inherits("QTipLabel") && top->isVisible()) {
                QVERIFY(top->grab().save(path("stock-tooltip", theme)));
            }
        }
        QToolTip::hideText();

        // Message bars: every severity, a title, a link, an action and text that wraps
        QWidget bars;
        bars.resize(640, 420);
        auto* barsLayout = new QVBoxLayout(&bars);
        const QList<QPair<VaultInfoBar::MessageType, QString>> barTypes = {
            {VaultInfoBar::Information, "Select a vault of your account to share."},
            {VaultInfoBar::Positive, "Remote sync 'Work' completed successfully!"},
            {VaultInfoBar::Warning,
             "This build is not signed for AutoFill, so AutoFill stays off. <a href=\"https://example.com\">Learn more</a>"},
            {VaultInfoBar::Error,
             "Failed to open Taxes.kdbx. It either does not exist or is not accessible, or another program is holding "
             "it open. Close the other program and try again."}};
        for (const auto& [type, text] : barTypes) {
            auto* bar = new VaultInfoBar(&bars);
            bar->setAnimate(false);
            if (type == VaultInfoBar::Error) {
                bar->setTitle("Couldn't open the database");
                bar->addAction(new QAction("Retry", bar));
            }
            bar->showMessage(text, type, VaultInfoBar::DisableAutoHide);
            barsLayout->addWidget(bar);
        }
        barsLayout->addStretch();
        bars.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bars));
        QTest::qWait(250);
        QVERIFY(bars.grab().save(path("stock-infobars", theme)));
        bars.hide();

        auto* calendar = dateTime->calendarWidget();
        calendar->resize(calendar->sizeHint());
        QVERIFY(calendar->grab().save(path("stock-calendar", theme)));
        gallery.hide();
    }

    config()->set(Config::GUI_ApplicationTheme, originalTheme);
    kpxcApp->applyTheme();
}
