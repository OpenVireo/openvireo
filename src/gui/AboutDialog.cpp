/*
 *  Copyright (C) 2012 Felix Geyer <debfx@fobos.de>
 *  Copyright (C) 2017 KeePassXC Team <team@keepassxc.org>
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

#include "AboutDialog.h"
#include "ui_AboutDialog.h"

#include "config-keepassx.h"
#include "core/Tools.h"
#include "crypto/Crypto.h"
#include "gui/Icons.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultSettingsPage.h"
#include "gui/vault/VaultWidgets.h"

#include <components/basicinput/HyperlinkButton.h>

#include <QClipboard>
#include <QFontDatabase>
#include <QScrollArea>
#include <QTabBar>
#include <QTimer>

namespace
{
    struct Maintainer
    {
        const char* name;
        const char* github;
        bool retired;
    };

    const Maintainer maintainers[] = {
        {"Bongani Mbigi", "bongani-m", false},
    };

    QUrl githubProfile(const Maintainer& maintainer)
    {
        return QUrl(QStringLiteral("https://github.com/") + QLatin1String(maintainer.github));
    }

    QString aboutMaintainers()
    {
        QString html = QStringLiteral("<p><ul>");
        for (const auto& maintainer : maintainers) {
            const auto name = QString::fromUtf8(maintainer.name);
            const auto github = QLatin1String(maintainer.github);
            const auto link = QStringLiteral("<a href=\"%1\">%2</a>").arg(githubProfile(maintainer).toString(), github);
            html += QStringLiteral("<li>");
            html += name == github ? link : QStringLiteral("%1 (%2)").arg(name.toHtmlEscaped(), link);
            if (maintainer.retired) {
                html += QStringLiteral(" (retired)");
            }
            html += QStringLiteral("</li>");
        }
        return html + QStringLiteral("</ul></p>");
    }
} // namespace

static const QString aboutContributors = QStringLiteral(
    "<p>OpenVireo is built by <a href=\"https://github.com/openvireo/openvireo/graphs/contributors\">its contributors</a>"
    " on GitHub.</p>"
    "<p>It is based on <a href=\"https://keepassxc.org/\">KeePassXC</a> and KeePassX, and includes the work of their"
    " <a href=\"https://github.com/keepassxreboot/keepassxc/graphs/contributors\">contributors</a> and translators.</p>");

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::AboutDialog())
{
    m_ui->setupUi(this);

    QString debugInfo = Tools::debugInfo().append("\n").append(Crypto::debugInfo());
    m_ui->debugInfo->setPlainText(debugInfo);

    m_ui->maintainers->setText(aboutMaintainers());
    m_ui->contributors->setText(aboutContributors);
    m_ui->contributors->setOpenExternalLinks(true);

    setAttribute(Qt::WA_DeleteOnClose);
    connect(m_ui->buttonBox, SIGNAL(rejected()), SLOT(close()));
    connect(m_ui->copyToClipboard, SIGNAL(clicked()), SLOT(copyToClipboard()));

    if (FluentTheme::vaultLayoutEnabled()) {
        setupVaultLayout();
        return;
    }

    resize(minimumSize());
    setWindowFlags(Qt::Sheet);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    m_ui->nameLabel->setText(m_ui->nameLabel->text().replace("${VERSION}", KEEPASSXC_VERSION));
    QFont nameLabelFont = m_ui->nameLabel->font();
    nameLabelFont.setPointSize(nameLabelFont.pointSize() + 4);
    m_ui->nameLabel->setFont(nameLabelFont);

    m_ui->iconLabel->setPixmap(icons()->applicationIcon().pixmap(48));

    m_ui->buttonBox->button(QDialogButtonBox::Close)->setDefault(true);
}

AboutDialog::~AboutDialog() = default;

void AboutDialog::copyToClipboard()
{
    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(m_ui->debugInfo->toPlainText());

    if (m_vaultCopyButton) {
        auto button = m_vaultCopyButton;
        button->setText(tr("Copied"));
        button->setIconGlyph(Typography::Icons::CheckMark);
        QTimer::singleShot(2000, button, [button] {
            button->setText(tr("Copy"));
            button->setIconGlyph(Typography::Icons::Copy);
        });
    }
}

// In the Vault Index layout, the dialog is the content of a modal over the window, which provides the title and the
// Close button. The app reads as a hero at the top, with its pages below it as a Fluent pivot of settings cards.
void AboutDialog::setupVaultLayout()
{
    setWindowFlags(Qt::Widget);
    m_ui->buttonBox->hide();
    m_ui->verticalLayout_2->setContentsMargins(0, 0, 0, 0);
    m_ui->verticalLayout_2->setSpacing(0);

    setupVaultHero();
    setupVaultPivot();
    setupVaultAboutPage();
    setupVaultContributorsPage();
    setupVaultDebugPage();
}

// The icon, name and version side by side on an inset panel, like the app header of Windows Settings' About page
void AboutDialog::setupVaultHero()
{
    auto ui = m_ui.data();

    auto hero = new QFrame(this);
    hero->setObjectName("aboutHero");

    ui->iconLabel->setFixedSize(64, 64);
    ui->iconLabel->setPixmap(icons()->applicationIcon().pixmap(64));

    ui->nameLabel->setText(QStringLiteral("OpenVireo"));
    ui->nameLabel->setIndent(0);
    auto version = new QLabel(tr("Version %1").arg(KEEPASSXC_VERSION), hero);
    version->setObjectName("aboutVersion");
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto tagline = new QLabel(tr("Cross-Platform Password Manager"), hero);
    tagline->setObjectName("aboutTagline");

    auto texts = new QVBoxLayout();
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(2);
    texts->addStretch();
    texts->addWidget(ui->nameLabel);
    texts->addWidget(version);
    texts->addWidget(tagline);
    texts->addStretch();

    auto layout = new QHBoxLayout(hero);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(16);
    layout->addWidget(ui->iconLabel, 0, Qt::AlignVCenter);
    layout->addLayout(texts, 1);

    Vault::followTheme(hero, [hero, name = ui->nameLabel, version, tagline] {
        const auto p = FluentTheme::palette();
        const auto argb = [](const QColor& color) { return color.name(QColor::HexArgb); };
        hero->setStyleSheet(QStringLiteral("#aboutHero { background-color: %1; border: 1px solid %2;"
                                           " border-radius: 8px; }")
                                .arg(argb(p.inset), argb(p.stroke)));
        Vault::styleLabel(name, Typography::FontRole::Title, p.text);
        Vault::styleLabel(version, Typography::FontRole::BodyStrong, p.text);
        Vault::styleLabel(tagline, Typography::FontRole::Caption, p.textSecondary);
    });

    auto oldLayout = ui->horizontalLayout;
    ui->verticalLayout_2->insertWidget(ui->verticalLayout_2->indexOf(oldLayout), hero);
    ui->verticalLayout_2->removeItem(oldLayout);
    delete oldLayout;
}

// The tabs read as a Fluent pivot: plain labels with an accent line under the current one. A separate tab bar
// switches the tab widget, as the style draws a tab widget's own bar on a filled base.
void AboutDialog::setupVaultPivot()
{
    auto tabs = m_ui->tabWidget;
    auto pivot = new QTabBar(this);
    pivot->setObjectName("aboutPivot");
    pivot->setAccessibleName(tr("About pages"));
    pivot->setDrawBase(false);
    pivot->setExpanding(false);
    for (int i = 0; i < tabs->count(); ++i) {
        pivot->addTab(tabs->tabText(i));
    }
    pivot->setCurrentIndex(tabs->currentIndex());
    connect(pivot, &QTabBar::currentChanged, tabs, &QTabWidget::setCurrentIndex);
    connect(tabs, &QTabWidget::currentChanged, pivot, &QTabBar::setCurrentIndex);
    tabs->tabBar()->hide();
    tabs->setDocumentMode(true);
    tabs->setStyleSheet(QStringLiteral("QTabWidget::pane { border: none; }"));
    Vault::followTheme(pivot, [pivot] { pivot->setStyleSheet(Vault::pivotStyleSheet()); });

    auto layout = m_ui->verticalLayout_2;
    layout->insertSpacing(layout->indexOf(tabs), 12);
    layout->insertWidget(layout->indexOf(tabs), pivot, 0, Qt::AlignLeft);
    layout->insertSpacing(layout->indexOf(tabs), 4);
}

namespace
{
    fluent::basicinput::HyperlinkButton* hyperlink(const QString& text, const QString& url, QWidget* parent)
    {
        auto link = new fluent::basicinput::HyperlinkButton(text, QUrl(url), parent);
        link->setToolTip(url);
        return link;
    }

    // Lets the cards of a page scroll once the modal is shorter than they are
    void scrollCards(QWidget* page)
    {
        auto cards = page->findChild<QWidget*>(page->objectName() + QStringLiteral("Cards"), Qt::FindDirectChildrenOnly);
        Q_ASSERT(cards);
        auto scrollArea = new QScrollArea(page);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        page->layout()->replaceWidget(cards, scrollArea);
        scrollArea->setWidget(cards);
        scrollArea->viewport()->setAutoFillBackground(false);
        cards->setAutoFillBackground(false);
    }
} // namespace

// Links, the license and the people behind the project as settings cards
void AboutDialog::setupVaultAboutPage()
{
    auto ui = m_ui.data();
    VaultSettingsPage page(ui->aboutTab, {});

    auto links = page.addSection(tr("Links"));
    links->addSetting(tr("Website"),
                      hyperlink(QStringLiteral("openvireo.com"), QStringLiteral("https://openvireo.com/"), this),
                      tr("News, documentation and downloads"));
    links->addSetting(tr("Report a bug"),
                      hyperlink(tr("GitHub issues"),
                                QStringLiteral("https://github.com/openvireo/openvireo/issues"),
                                this),
                      tr("Include the debug info when you report a bug"));
    links->addSetting(tr("Contribute"),
                      hyperlink(tr("Contributing guide"),
                                QStringLiteral("https://github.com/openvireo/openvireo/blob/develop/.github/CONTRIBUTING.md"),
                                this),
                      tr("Code, translations and bug reports are welcome"));

    auto license = page.addSection(tr("License"));
    license->addSetting(tr("GNU General Public License"),
                        hyperlink(tr("Read the license"), QStringLiteral("https://www.gnu.org/licenses/"), this),
                        ui->label_3->text());

    auto team = page.addSection(tr("Maintainers"));
    auto maintainersCard = team->addSetting(tr("The OpenVireo maintainers"), QWidgetList{}, ui->label_7->text());
    maintainersCard->setCollapsible(false);
    for (const auto& maintainer : maintainers) {
        auto name = QString::fromUtf8(maintainer.name);
        if (maintainer.retired) {
            name = tr("%1 (retired)").arg(name);
        }
        maintainersCard->addSetting(
            name, hyperlink(QStringLiteral("@") + QLatin1String(maintainer.github), githubProfile(maintainer).toString(), this));
    }

    page.finish();
    scrollCards(ui->aboutTab);
}

// Each group of contributors as a section of its own, the names flowing across a card
void AboutDialog::setupVaultContributorsPage()
{
    auto ui = m_ui.data();
    VaultSettingsPage page(ui->contribTab, {});

    auto code = page.addSection();
    code->addSetting(tr("Everyone who contributed code"),
                     hyperlink(tr("See contributions on GitHub"),
                               QStringLiteral("https://github.com/openvireo/openvireo/graphs/contributors"),
                               this),
                     tr("OpenVireo is built by its community"));

    auto upstream = page.addSection(tr("Based on KeePassXC"));
    upstream->addSetting(tr("KeePassXC and KeePassX contributors"),
                         hyperlink(tr("See contributions on GitHub"),
                                   QStringLiteral("https://github.com/keepassxreboot/keepassxc/graphs/contributors"),
                                   this),
                         tr("OpenVireo includes the work of everyone who contributed code and translations upstream"));

    page.finish();
    scrollCards(ui->contribTab);
}

// The debug info in a monospaced field, with a button to copy it for a bug report
void AboutDialog::setupVaultDebugPage()
{
    auto ui = m_ui.data();
    VaultSettingsPage page(ui->debugTab, {});

    auto button = new VaultButton(tr("Copy"), this);
    button->setObjectName("vaultCopyDebugInfo");
    button->setIconGlyph(Typography::Icons::Copy);
    connect(button, &QPushButton::clicked, this, &AboutDialog::copyToClipboard);
    m_vaultCopyButton = button;
    ui->copyToClipboard->hide();

    auto section = page.addSection();
    section->addSetting(tr("System information"), button, ui->label->text());

    ui->debugInfo->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ui->debugInfo->setLineWrapMode(QPlainTextEdit::NoWrap);
    ui->debugInfo->setMinimumHeight(180);
    section->addWidget(ui->debugInfo);

    page.finish();
    scrollCards(ui->debugTab);
}
