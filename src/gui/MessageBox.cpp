/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2013 Felix Geyer <debfx@fobos.de>
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

#include "MessageBox.h"

#include "gui/styles/FluentTheme.h"

#include <components/basicinput/Button.h>
#include <components/dialogs_flyouts/ContentDialog.h>
#include <components/textfields/Label.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLayout>
#include <QMap>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWindow>

namespace
{
    // Width of the alert card, matching WinUI's content dialogs
    constexpr int FluentAlertWidth = 448;
} // namespace

QWindow* MessageBox::m_overrideParent(nullptr);

MessageBox::Button MessageBox::m_nextAnswer(MessageBox::NoButton);

QHash<QAbstractButton*, MessageBox::Button> MessageBox::m_addedButtonLookup =
    QHash<QAbstractButton*, MessageBox::Button>();

QMap<MessageBox::Button, std::pair<QString, QMessageBox::ButtonRole>> MessageBox::m_buttonDefs =
    QMap<MessageBox::Button, std::pair<QString, QMessageBox::ButtonRole>>();

void MessageBox::initializeButtonDefs()
{
    m_buttonDefs = QMap<Button, std::pair<QString, QMessageBox::ButtonRole>>{
        // Reimplementation of Qt StandardButtons
        {Ok, {stdButtonText(QMessageBox::Ok), QMessageBox::ButtonRole::AcceptRole}},
        {Open, {stdButtonText(QMessageBox::Open), QMessageBox::ButtonRole::AcceptRole}},
        {Save, {stdButtonText(QMessageBox::Save), QMessageBox::ButtonRole::AcceptRole}},
        {Cancel, {stdButtonText(QMessageBox::Cancel), QMessageBox::ButtonRole::RejectRole}},
        {Close, {stdButtonText(QMessageBox::Close), QMessageBox::ButtonRole::RejectRole}},
        {Discard, {stdButtonText(QMessageBox::Discard), QMessageBox::ButtonRole::DestructiveRole}},
        {Apply, {stdButtonText(QMessageBox::Apply), QMessageBox::ButtonRole::ApplyRole}},
        {Reset, {stdButtonText(QMessageBox::Reset), QMessageBox::ButtonRole::ResetRole}},
        {RestoreDefaults, {stdButtonText(QMessageBox::RestoreDefaults), QMessageBox::ButtonRole::ResetRole}},
        {Help, {stdButtonText(QMessageBox::Help), QMessageBox::ButtonRole::HelpRole}},
        {SaveAll, {stdButtonText(QMessageBox::SaveAll), QMessageBox::ButtonRole::AcceptRole}},
        {Yes, {stdButtonText(QMessageBox::Yes), QMessageBox::ButtonRole::YesRole}},
        {YesToAll, {stdButtonText(QMessageBox::YesToAll), QMessageBox::ButtonRole::YesRole}},
        {No, {stdButtonText(QMessageBox::No), QMessageBox::ButtonRole::NoRole}},
        {NoToAll, {stdButtonText(QMessageBox::NoToAll), QMessageBox::ButtonRole::NoRole}},
        {Abort, {stdButtonText(QMessageBox::Abort), QMessageBox::ButtonRole::RejectRole}},
        {Retry, {stdButtonText(QMessageBox::Retry), QMessageBox::ButtonRole::AcceptRole}},
        {Ignore, {stdButtonText(QMessageBox::Ignore), QMessageBox::ButtonRole::AcceptRole}},

        // KeePassXC Buttons
        {Overwrite, {QMessageBox::tr("Overwrite"), QMessageBox::ButtonRole::AcceptRole}},
        {Delete, {QMessageBox::tr("Delete"), QMessageBox::ButtonRole::AcceptRole}},
        {Move, {QMessageBox::tr("Move"), QMessageBox::ButtonRole::AcceptRole}},
        {Empty, {QMessageBox::tr("Empty"), QMessageBox::ButtonRole::AcceptRole}},
        {Remove, {QMessageBox::tr("Remove"), QMessageBox::ButtonRole::AcceptRole}},
        {Skip, {QMessageBox::tr("Skip"), QMessageBox::ButtonRole::AcceptRole}},
        {Disable, {QMessageBox::tr("Disable"), QMessageBox::ButtonRole::AcceptRole}},
        {Merge, {QMessageBox::tr("Merge"), QMessageBox::ButtonRole::AcceptRole}},
        {Continue, {QMessageBox::tr("Continue"), QMessageBox::ButtonRole::AcceptRole}},
        {ContinueWithWeakPass, {QMessageBox::tr("Continue with weak password"), QMessageBox::ButtonRole::AcceptRole}},
    };
}

QString MessageBox::stdButtonText(QMessageBox::StandardButton button)
{
    QMessageBox buttonHost;
    return buttonHost.addButton(button)->text();
}

MessageBox::Button MessageBox::messageBox(QWidget* parent,
                                          QMessageBox::Icon icon,
                                          const QString& title,
                                          const QString& text,
                                          MessageBox::Buttons buttons,
                                          MessageBox::Button defaultButton,
                                          MessageBox::Action action,
                                          QCheckBox* checkbox)
{
    if (m_nextAnswer == MessageBox::NoButton) {
        Button fluentAnswer = NoButton;
        if (fluentMessageBox(parent, title, text, buttons, defaultButton, action, checkbox, fluentAnswer)) {
            return fluentAnswer;
        }

        QMessageBox msgBox(parent);
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setIcon(icon);
        msgBox.setWindowTitle(title);
        // Replace newlines with HTML line breaks
        auto fixedText = text;
        msgBox.setText(fixedText.replace("\n", "<br>"));

        if (m_overrideParent) {
            // Force the creation of the QWindow, without this windowHandle() will return nullptr
            msgBox.winId();
            auto msgBoxWindow = msgBox.windowHandle();
            Q_ASSERT(msgBoxWindow);
            msgBoxWindow->setTransientParent(m_overrideParent);
        }

        for (uint64_t b = First; b <= Last; b <<= 1) {
            if (b & buttons) {
                QString buttonText = m_buttonDefs[static_cast<Button>(b)].first;
                QMessageBox::ButtonRole role = m_buttonDefs[static_cast<Button>(b)].second;

                auto buttonPtr = msgBox.addButton(buttonText, role);
                m_addedButtonLookup.insert(buttonPtr, static_cast<Button>(b));
            }
        }

        if (defaultButton != MessageBox::NoButton) {
            QList<QAbstractButton*> defPtrList = m_addedButtonLookup.keys(defaultButton);
            if (defPtrList.count() > 0) {
                msgBox.setDefaultButton(static_cast<QPushButton*>(defPtrList[0]));
            }
        }

        if (checkbox) {
            checkbox->setParent(&msgBox);
            msgBox.setCheckBox(checkbox);
        }

        if (action == MessageBox::Raise) {
            msgBox.setWindowFlags(Qt::WindowStaysOnTopHint);
            msgBox.activateWindow();
            msgBox.raise();
        }
        msgBox.layout()->setSizeConstraint(QLayout::SetMinimumSize);
        msgBox.exec();

        Button returnButton = m_addedButtonLookup[msgBox.clickedButton()];
        m_addedButtonLookup.clear();
        return returnButton;

    } else {
        MessageBox::Button returnButton = m_nextAnswer;
        m_nextAnswer = MessageBox::NoButton;
        return returnButton;
    }
}

bool MessageBox::fluentMessageBox(QWidget* parent,
                                  const QString& title,
                                  const QString& text,
                                  MessageBox::Buttons buttons,
                                  MessageBox::Button defaultButton,
                                  MessageBox::Action action,
                                  QCheckBox* checkbox,
                                  MessageBox::Button& answer)
{
    // The alert is drawn inside the parent's window, so it needs a visible one. Prompts for other applications'
    // windows and prompts that raise themselves above other applications stay native message boxes.
    if (!FluentTheme::vaultLayoutEnabled() || m_overrideParent || action == MessageBox::Raise || !parent
        || !parent->window()->isVisible()) {
        return false;
    }

    // A content dialog has primary, secondary and close commands. The button that rejects goes to close, which is
    // also what Escape answers, and the others fill primary, secondary and any further commands.
    QList<Button> commands;
    Button closeButton = NoButton;
    for (uint64_t b = First; b <= Last; b <<= 1) {
        if (!(b & buttons)) {
            continue;
        }
        if (m_buttonDefs[static_cast<Button>(b)].second == QMessageBox::RejectRole && closeButton == NoButton) {
            closeButton = static_cast<Button>(b);
        } else {
            commands << static_cast<Button>(b);
        }
    }
    if (commands.isEmpty() && closeButton == NoButton) {
        return false;
    }
    // The default command leads, as the dialog's primary command
    if (commands.removeOne(defaultButton)) {
        commands.prepend(defaultButton);
    }

    using fluent::dialogs_flyouts::ContentDialog;
    ContentDialog dialog(parent->window());
    dialog.setThemeSource(parent->window());
    dialog.setTitle(title);
    dialog.setPrimaryButtonText(m_buttonDefs.value(commands.value(0, NoButton)).first);
    dialog.setSecondaryButtonText(m_buttonDefs.value(commands.value(1, NoButton)).first);
    dialog.setCloseButtonText(m_buttonDefs.value(closeButton).first);

    // Each button maps to the dialog result that answers with it
    QHash<int, Button> results;
    if (!commands.isEmpty()) {
        results.insert(ContentDialog::ResultPrimary, commands[0]);
    }
    if (commands.size() > 1) {
        results.insert(ContentDialog::ResultSecondary, commands[1]);
    }

    // Further commands join the button bar ahead of the close button, so a dialog can offer more than two choices
    QHash<Button, fluent::basicinput::Button*> extraButtons;
    if (commands.size() > 2) {
        auto bar = dialog.findChildren<fluent::basicinput::Button*>().value(0)->parentWidget();
        auto barLayout = qobject_cast<QHBoxLayout*>(bar->layout());
        Q_ASSERT(barLayout);
        for (int i = 2; i < commands.size(); ++i) {
            auto button = new fluent::basicinput::Button(m_buttonDefs.value(commands[i]).first, bar);
            button->setMinimumWidth(96);
            barLayout->insertWidget(i, button, 1);
            const int result = ContentDialog::ResultSecondary + i - 1;
            results.insert(result, commands[i]);
            extraButtons.insert(commands[i], button);
            QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, result] { dialog.done(result); });
        }
    }

    auto content = new QWidget();
    auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto message = new fluent::textfields::Label(content);
    message->setFluentTypography(Typography::FontRole::Body);
    message->setTextColorRole(fluent::textfields::Label::TextColorRole::Primary);
    message->setTextFormat(Qt::RichText);
    message->setWordWrap(true);
    message->setOpenExternalLinks(true);
    message->setTextInteractionFlags(Qt::TextBrowserInteraction);
    auto fixedText = text;
    message->setText(fixedText.replace("\n", "<br>"));
    layout->addWidget(message);
    if (checkbox) {
        layout->addWidget(checkbox);
    }
    dialog.setContent(content);
    // Each button past three needs about another button's width
    const int extraWidth = qMax(0, commands.size() + (closeButton != NoButton ? 1 : 0) - 3) * 120;
    dialog.resize(FluentAlertWidth + extraWidth + 2 * dialog.shadowSize(), dialog.minimumHeight());

    // Without a button that rejects, Escape answers like a message box would: with the only button, or with No
    Button escapeButton = closeButton;
    if (escapeButton == NoButton) {
        if (commands.size() == 1) {
            escapeButton = commands[0];
        } else {
            for (auto button : commands) {
                if (m_buttonDefs.value(button).second == QMessageBox::NoRole) {
                    escapeButton = button;
                    break;
                }
            }
        }
    }
    if (escapeButton == NoButton) {
        dialog.setClosePolicy(fluent::dialogs_flyouts::Dialog::NoAutoClose);
    }

    // The default button is the accent one, and Enter presses it
    if (defaultButton != NoButton && (commands.contains(defaultButton) || closeButton == defaultButton)) {
        if (extraButtons.contains(defaultButton)) {
            extraButtons[defaultButton]->setFluentStyle(fluent::basicinput::Button::Accent);
        } else {
            dialog.setDefaultButton(defaultButton == closeButton         ? ContentDialog::Close
                                    : defaultButton == commands.value(0) ? ContentDialog::Primary
                                                                         : ContentDialog::Secondary);
        }
        const auto defaultText = m_buttonDefs.value(defaultButton).first;
        for (auto button : dialog.findChildren<QPushButton*>()) {
            // Not focused: the dialog takes focus on opening, and a button losing focus stops being the default
            if (button->text() == defaultText) {
                button->setDefault(true);
            }
        }
    }

    const int result = dialog.exec();
    answer = results.value(result, escapeButton);
    return true;
}

MessageBox::Button MessageBox::critical(QWidget* parent,
                                        const QString& title,
                                        const QString& text,
                                        MessageBox::Buttons buttons,
                                        MessageBox::Button defaultButton,
                                        MessageBox::Action action,
                                        QCheckBox* checkbox)
{
    return messageBox(parent, QMessageBox::Critical, title, text, buttons, defaultButton, action, checkbox);
}

MessageBox::Button MessageBox::warning(QWidget* parent,
                                       const QString& title,
                                       const QString& text,
                                       MessageBox::Buttons buttons,
                                       MessageBox::Button defaultButton,
                                       MessageBox::Action action,
                                       QCheckBox* checkbox)
{
    return messageBox(parent, QMessageBox::Warning, title, text, buttons, defaultButton, action, checkbox);
}

MessageBox::Button MessageBox::information(QWidget* parent,
                                           const QString& title,
                                           const QString& text,
                                           MessageBox::Buttons buttons,
                                           MessageBox::Button defaultButton,
                                           MessageBox::Action action,
                                           QCheckBox* checkbox)
{
    return messageBox(parent, QMessageBox::Information, title, text, buttons, defaultButton, action, checkbox);
}

MessageBox::Button MessageBox::question(QWidget* parent,
                                        const QString& title,
                                        const QString& text,
                                        MessageBox::Buttons buttons,
                                        MessageBox::Button defaultButton,
                                        MessageBox::Action action,
                                        QCheckBox* checkbox)
{
    return messageBox(parent, QMessageBox::Question, title, text, buttons, defaultButton, action, checkbox);
}

void MessageBox::setNextAnswer(MessageBox::Button button)
{
    m_nextAnswer = button;
}

MessageBox::OverrideParent::OverrideParent(QWindow* newParent)
    : m_oldParent(MessageBox::m_overrideParent)
{
    MessageBox::m_overrideParent = newParent;
}

MessageBox::OverrideParent::~OverrideParent()
{
    MessageBox::m_overrideParent = m_oldParent;
}

MessageBox::OverrideButtonText::OverrideButtonText(MessageBox::Button button, const QString& text)
    : m_button(button)
    , m_oldText(MessageBox::m_buttonDefs[button].first)
{
    MessageBox::m_buttonDefs[button].first = text;
}

MessageBox::OverrideButtonText::~OverrideButtonText()
{
    MessageBox::m_buttonDefs[m_button].first = m_oldText;
}
