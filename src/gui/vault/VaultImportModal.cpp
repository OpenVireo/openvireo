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

#include "VaultImportModal.h"

#include "gui/wizard/ImportWizard.h"
#include "gui/wizard/ImportWizardPageSelect.h"

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

VaultImportModal::VaultImportModal(QWidget* window, ImportWizard* wizard)
    : VaultModal(window, new QWidget(), tr("Import"), QStringLiteral("vaultImport"))
    , m_wizard(wizard)
    , m_pageSelect(wizard->findChild<ImportWizardPageSelect*>())
{
    Q_ASSERT(m_pageSelect);

    m_stepList = new VaultStepList({tr("Source"), tr("Details"), tr("Review")}, card());
    m_stepList->setObjectName("vaultImportSteps");
    m_stepList->onStepClicked = [this](int step) {
        while (m_current > step) {
            back();
        }
    };
    bodyLayout()->insertWidget(0, m_stepList);

    m_stepTitle = new QLabel(content());
    m_stepTitle->setObjectName("vaultImportStepTitle");
    m_stepTitle->setFont(pixelFont(font(), 18, QFont::Bold));

    m_stepSubtitle = new QLabel(content());
    m_stepSubtitle->setObjectName("vaultImportStepSubtitle");
    m_stepSubtitle->setFont(pixelFont(font(), 14));
    m_stepSubtitle->setWordWrap(true);
    m_stepSubtitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_wizard->setWindowFlags(Qt::Widget);
    m_wizard->setParent(content());

    auto layout = new QVBoxLayout(content());
    layout->setContentsMargins(8, 4, 8, 0);
    layout->setSpacing(6);
    layout->addWidget(m_stepTitle);
    layout->addWidget(m_stepSubtitle);
    layout->addSpacing(6);
    layout->addWidget(m_wizard, 1);

    auto cancelButton = addFooterButton(tr("Cancel"), false, QStringLiteral("Cancel"));
    connect(cancelButton, &QPushButton::clicked, this, &VaultImportModal::reject);
    m_backButton = addFooterButton(tr("Back"), false, QStringLiteral("Back"));
    connect(m_backButton, &QPushButton::clicked, this, &VaultImportModal::back);
    m_nextButton = addFooterButton(tr("Continue"), true, QStringLiteral("Continue"));
    connect(m_nextButton, &QPushButton::clicked, this, &VaultImportModal::next);
    // Wide enough for either label, so the footer doesn't shift on the last step
    const auto metrics = m_nextButton->fontMetrics();
    m_nextButton->setMinimumWidth(
        qMax(metrics.horizontalAdvance(tr("Import")), metrics.horizontalAdvance(tr("Continue"))) + 48);

    connect(m_pageSelect, &ImportWizardPageSelect::sourceActivated, this, &VaultImportModal::next);
    connect(m_wizard, &QWizard::currentIdChanged, this, &VaultImportModal::updateNextButton);
    for (int id : m_wizard->pageIds()) {
        connect(m_wizard->page(id), &QWizardPage::completeChanged, this, &VaultImportModal::updateNextButton);
    }
    // Finishing or cancelling the wizard is the end of the modal. Connected before the import itself, so the modal
    // is gone before importing into a new database opens the New Database modal.
    connect(m_wizard, &QDialog::finished, this, [this] {
        close();
        deleteLater();
    });

    setMaximumCardWidth(940);
    setInitialFocus(m_pageSelect->findChild<QListWidget*>("importTypeList"));
}

VaultImportModal::~VaultImportModal() = default;

int VaultImportModal::currentStep() const
{
    return m_current;
}

void VaultImportModal::next()
{
    if (!m_nextButton->isEnabled()) {
        return;
    }

    switch (m_current) {
    case SourceStep:
        m_pageSelect->setSourceStep(false);
        showStep(DetailsStep);
        break;
    case DetailsStep:
        // Reads the source into the review page, which shows any error itself
        m_wizard->next();
        if (m_wizard->currentPage() != m_pageSelect) {
            // The review page builds its widgets anew, so give them the card's colors
            applyColors();
            showStep(ReviewStep);
        }
        break;
    default:
        // The wizard checks the page once more, then hands the database over
        m_wizard->accept();
        break;
    }
}

void VaultImportModal::back()
{
    switch (m_current) {
    case DetailsStep:
        m_pageSelect->setSourceStep(true);
        showStep(SourceStep);
        break;
    case ReviewStep:
        m_wizard->back();
        showStep(DetailsStep);
        break;
    default:
        break;
    }
}

void VaultImportModal::accept()
{
    next();
}

void VaultImportModal::reject()
{
    m_wizard->reject();
}

void VaultImportModal::prepare()
{
    m_pageSelect->setSourceStep(true);
    showStep(SourceStep);
}

void VaultImportModal::showStep(int step)
{
    m_current = step;
    switch (step) {
    case SourceStep:
        m_stepTitle->setText(tr("Choose a source"));
        m_stepSubtitle->setText(tr("Where are the passwords you want to import?"));
        break;
    case DetailsStep:
        m_stepTitle->setText(m_pageSelect->sourceName());
        m_stepSubtitle->setText(tr("Choose what to import and where its entries go."));
        break;
    default:
        m_stepTitle->setText(tr("Review"));
        m_stepSubtitle->setText(m_wizard->importType() == ImportWizard::IMPORT_CSV
                                    ? tr("Match the columns of the file to the fields of the entries.")
                                    : tr("Check the entries before they are imported."));
        break;
    }
    m_stepList->setCurrent(step);
    m_backButton->setVisible(step > SourceStep);
    m_nextButton->setText(step == ReviewStep ? tr("Import") : tr("Continue"));
    updateNextButton();

    if (!isOpen()) {
        return;
    }
    // The tiles keep the focus on the first step, so the arrow keys move between them
    if (step == SourceStep) {
        m_pageSelect->findChild<QListWidget*>("importTypeList")->setFocus(Qt::OtherFocusReason);
        return;
    }
    auto focused = window()->focusWidget();
    if (!(focused && focused->isVisible() && m_wizard->isAncestorOf(focused))) {
        m_nextButton->setFocus(Qt::OtherFocusReason);
    }
}

void VaultImportModal::updateNextButton()
{
    // Any source can be chosen; the details and the review need what the wizard's pages ask for. Cancelling resets
    // the wizard, which leaves it without a page for a moment.
    auto page = m_wizard->currentPage();
    m_nextButton->setEnabled(m_current == SourceStep || (page && page->isComplete()));
}

void VaultImportModal::applyColors()
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
