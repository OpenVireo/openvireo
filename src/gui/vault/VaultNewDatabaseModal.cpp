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

#include "VaultNewDatabaseModal.h"

#include "core/Database.h"
#include "core/Global.h"
#include "core/Group.h"
#include "gui/dbsettings/DatabaseSettingsWidgetDatabaseKey.h"
#include "gui/dbsettings/DatabaseSettingsWidgetEncryption.h"
#include "gui/dbsettings/DatabaseSettingsWidgetMetaDataSimple.h"
#include "gui/vault/VaultControls.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>

VaultNewDatabaseModal::VaultNewDatabaseModal(QWidget* window)
    : VaultModal(window, new QWidget(), tr("New Database"), QStringLiteral("vaultNewDatabase"))
{
    // Same pages and wording as the New Database wizard
    m_steps = {
        {tr("General"),
         tr("Please fill in the display name and an optional description for your new database:"),
         new DatabaseSettingWidgetMetaData()},
        {tr("Encryption"),
         tr("Here you can adjust the database encryption settings. "
            "Don't worry, you can change them later in the database settings."),
         new DatabaseSettingsWidgetEncryption()},
        {tr("Database Credentials"),
         tr("A set of credentials known only to you that protects your database."),
         new DatabaseSettingsWidgetDatabaseKey()},
    };

    QStringList titles;
    for (const auto& step : m_steps) {
        titles << step.title;
    }
    m_stepList = new VaultStepList(titles, card());
    m_stepList->setObjectName("vaultNewDatabaseSteps");
    m_stepList->onStepClicked = [this](int step) { showStep(step, false); };
    bodyLayout()->insertWidget(0, m_stepList);

    m_stepTitle = new QLabel(content());
    m_stepTitle->setObjectName("vaultNewDatabaseStepTitle");
    m_stepTitle->setFont(pixelFont(font(), 18, QFont::Bold));

    m_stepSubtitle = new QLabel(content());
    m_stepSubtitle->setObjectName("vaultNewDatabaseStepSubtitle");
    m_stepSubtitle->setFont(pixelFont(font(), 14));
    m_stepSubtitle->setWordWrap(true);
    // Room for two lines, so a long subtitle isn't squeezed by the page below it
    m_stepSubtitle->setMinimumHeight(QFontMetrics(m_stepSubtitle->font()).lineSpacing() * 2);
    m_stepSubtitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_stack = new QStackedWidget(content());
    m_stack->setObjectName("vaultNewDatabasePages");
    m_stack->setProperty(VaultLineEdit::LargePaddingProperty, true);
    for (const auto& step : asConst(m_steps)) {
        auto scroll = new QScrollArea(m_stack);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->viewport()->setAutoFillBackground(false);
        scroll->setWidget(step.page);
        step.page->setAutoFillBackground(false);
        m_stack->addWidget(scroll);
    }

    auto layout = new QVBoxLayout(content());
    layout->setContentsMargins(8, 4, 8, 0);
    layout->setSpacing(6);
    layout->addWidget(m_stepTitle);
    layout->addWidget(m_stepSubtitle);
    layout->addSpacing(10);
    layout->addWidget(m_stack, 1);

    auto cancelButton = addFooterButton(tr("Cancel"), false, QStringLiteral("Cancel"));
    connect(cancelButton, &QPushButton::clicked, this, &VaultNewDatabaseModal::reject);
    m_backButton = addFooterButton(tr("Back"), false, QStringLiteral("Back"));
    connect(m_backButton, &QPushButton::clicked, this, &VaultNewDatabaseModal::back);
    m_nextButton = addFooterButton(tr("Continue"), true, QStringLiteral("Continue"));
    connect(m_nextButton, &QPushButton::clicked, this, &VaultNewDatabaseModal::next);
    // Wide enough for either label, so the footer doesn't shift on the last step
    const auto metrics = m_nextButton->fontMetrics();
    m_nextButton->setMinimumWidth(
        qMax(metrics.horizontalAdvance(tr("Create Database")), metrics.horizontalAdvance(tr("Continue"))) + 48);

    setMaximumCardWidth(880);
    setInitialFocus(m_steps.first().page->findChild<QLineEdit*>("databaseName"));
}

VaultNewDatabaseModal::~VaultNewDatabaseModal() = default;

int VaultNewDatabaseModal::currentStep() const
{
    return m_current;
}

int VaultNewDatabaseModal::stepCount() const
{
    return m_steps.size();
}

void VaultNewDatabaseModal::next()
{
    // Like the wizard, a step is saved into the database before moving on
    auto page = m_steps[m_current].page;
    if (!page->saveSettings()) {
        return;
    }
    page->uninitialize();

    if (m_current + 1 < m_steps.size()) {
        showStep(m_current + 1, true);
        return;
    }

    auto db = m_db;
    m_db.reset();
    close();
    emit finished(db);
}

void VaultNewDatabaseModal::back()
{
    if (m_current > 0) {
        showStep(m_current - 1, false);
    }
}

void VaultNewDatabaseModal::accept()
{
    next();
}

void VaultNewDatabaseModal::reject()
{
    m_db.reset();
    close();
    emit finished({});
}

void VaultNewDatabaseModal::prepare()
{
    // Each opening starts over with an empty database, as the wizard does
    m_db = QSharedPointer<Database>::create();
    m_db->rootGroup()->setName(tr("Root", "Root group"));
    m_db->setKdf({});
    m_db->setKey({});
    showStep(0, true);
}

void VaultNewDatabaseModal::showStep(int index, bool load)
{
    m_current = index;
    const auto& step = m_steps[index];
    // Going forward loads the step from the database; going back keeps what was entered
    if (load) {
        step.page->loadSettings(m_db);
    }

    m_stack->setCurrentIndex(index);
    m_stepTitle->setText(step.title);
    m_stepSubtitle->setText(step.subtitle);
    m_stepList->setCurrent(index);
    m_backButton->setVisible(index > 0);
    const bool last = index + 1 == m_steps.size();
    m_nextButton->setText(last ? tr("Create Database") : tr("Continue"));

    // Pages that focus a field of their own, such as the name or password, keep it
    auto focused = window()->focusWidget();
    if (isOpen() && !(focused && m_stack->currentWidget()->isAncestorOf(focused))) {
        m_nextButton->setFocus(Qt::OtherFocusReason);
    }
}

void VaultNewDatabaseModal::applyColors()
{
    VaultModal::applyColors();

    // The base class gives every child the card palette, so color the headings after it
    const auto colors = VaultModal::colors();
    QPalette titlePalette = m_stepTitle->palette();
    titlePalette.setColor(QPalette::WindowText, colors.text);
    m_stepTitle->setPalette(titlePalette);
    QPalette subtitlePalette = m_stepSubtitle->palette();
    subtitlePalette.setColor(QPalette::WindowText, colors.textSecondary);
    m_stepSubtitle->setPalette(subtitlePalette);
    m_stepList->update();
}
