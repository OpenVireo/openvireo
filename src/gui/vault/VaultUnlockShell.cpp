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

#include "VaultUnlockShell.h"

#include "gui/Icons.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    const QString Site = QStringLiteral("https://openvireo.com");

    // The dark panel is the same in light and dark mode, as on the account site
    struct HeroColors
    {
        QColor background;
        QColor text;
        QColor muted;
        QColor faint;
        QColor eyebrow;
        QColor line;
        QColor box;
        QColor dot;
        QColor done;
        QColor onDone;
    };

    HeroColors heroColors()
    {
        const auto p = FluentTheme::palette();
        if (p.highContrast) {
            // High contrast: the scheme's own colours, every line in the text colour
            return {p.canvas, p.text, p.text, p.text, p.text, p.text, p.canvas, p.canvas, p.accent, p.onAccent};
        }
        return {QColor(0x07, 0x10, 0x0a),
                QColor(0xee, 0xf2, 0xef),
                QColor(0xa3, 0xad, 0xa6),
                QColor(0x7f, 0x8a, 0x82),
                QColor(0xa6, 0xd9, 0x92),
                QColor(0x22, 0x29, 0x25),
                QColor(0x11, 0x17, 0x13),
                QColor(0x16, 0x24, 0x1a),
                QColor(0x3c, 0x7d, 0x20),
                QColor(Qt::white)};
    }

    QString rgba(const QColor& c)
    {
        return c.name(QColor::HexArgb);
    }

    QString linkHtml(const QString& href, const QString& text, const QColor& color)
    {
        return QStringLiteral("<a href=\"%1\" style=\"color: %2; text-decoration: underline\">%3</a>")
            .arg(href, color.name(), text.toHtmlEscaped());
    }
} // namespace

VaultUnlockSteps::VaultUnlockSteps(QWidget* parent)
    : QFrame(parent)
    , m_layout(new QVBoxLayout(this))
{
    setObjectName("vaultUnlockSteps");
    setAccessibleName(tr("Unlock progress"));
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
}

void VaultUnlockSteps::setSteps(const QList<Step>& steps)
{
    for (const auto& row : m_rows) {
        delete row.row;
    }
    m_rows.clear();
    m_steps = steps;
    for (int i = 0; i < steps.size(); ++i) {
        auto* row = new QWidget(this);
        row->setObjectName(QStringLiteral("vaultUnlockStep%1").arg(i + 1));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(16);
        auto* dot = new QLabel(row);
        dot->setFixedSize(32, 32);
        dot->setAlignment(Qt::AlignCenter);
        auto* text = new QLabel(row);
        text->setWordWrap(true);
        text->setTextFormat(Qt::RichText);
        layout->addWidget(dot, 0, Qt::AlignVCenter);
        layout->addWidget(text, 1);
        m_layout->addWidget(row);
        m_rows.append({row, dot, text});
    }
    applyTheme();
}

void VaultUnlockSteps::setProgress(int step, bool busy)
{
    if (m_progress == step && m_busy == busy) {
        return;
    }
    m_progress = step;
    m_busy = busy;
    applyTheme();
}

int VaultUnlockSteps::progress() const
{
    return m_progress;
}

bool VaultUnlockSteps::isBusy() const
{
    return m_busy;
}

void VaultUnlockSteps::applyTheme()
{
    const auto c = heroColors();
    setStyleSheet(QStringLiteral("QFrame#vaultUnlockSteps { background-color: %1; border: 1px solid %2;"
                                 " border-radius: 20px; }")
                      .arg(rgba(c.box), rgba(c.line)));
    auto font = Vault::font(Typography::FontRole::Body);
    for (int i = 0; i < m_rows.size(); ++i) {
        const auto& r = m_rows.at(i);
        const bool done = i < m_progress;
        const bool current = i == m_progress;
        const bool busy = current && m_busy;
        // A rule above every step but the first
        r.row->setStyleSheet(
            i == 0 ? QString()
                   : QStringLiteral("QWidget#%1 { border-top: 1px solid %2; }").arg(r.row->objectName(), rgba(c.line)));
        r.row->setAttribute(Qt::WA_StyledBackground, true);

        QString dotStyle = QStringLiteral("QLabel { border-radius: 16px; background-color: %1; color: %2;")
                               .arg(rgba(done ? c.done : c.dot), rgba(done ? c.onDone : c.eyebrow));
        if (current) {
            dotStyle += QStringLiteral(" border: 1.5px solid %1;").arg(rgba(c.eyebrow));
        }
        dotStyle += QStringLiteral(" }");
        r.dot->setStyleSheet(dotStyle);
        auto dotFont = Vault::font(Typography::FontRole::BodyStrong);
        r.dot->setFont(dotFont);
        // A check, an ellipsis while running (no spinner, so nothing moves), or the step's number
        r.dot->setText(done ? QStringLiteral("✓") : busy ? QStringLiteral("…") : QString::number(i + 1));

        const auto title = m_steps.value(i).title;
        const auto text = m_steps.value(i).text;
        // Steps not reached yet are quieter (not in high contrast, where every colour is the text colour)
        const bool reached = done || current;
        r.text->setFont(font);
        r.text->setText(QStringLiteral("<span style=\"color: %1; font-weight: 600\">%2</span>"
                                       " <span style=\"color: %3\">%4</span>")
                            .arg((reached ? c.text : c.muted).name(),
                                 title.toHtmlEscaped(),
                                 (reached ? c.muted : c.faint).name(),
                                 text.toHtmlEscaped()));
        const QString state = done ? tr("done") : busy ? tr("in progress") : current ? tr("current") : tr("to do");
        r.row->setAccessibleName(QStringLiteral("%1 %2 (%3)").arg(title, text, state));
    }
}

VaultUnlockShell::VaultUnlockShell(QWidget* parent)
    : QWidget(parent)
    , m_hero(new QFrame(this))
    , m_logo(new QLabel(m_hero))
    , m_brand(new QLabel(tr("OpenVireo"), m_hero))
    , m_eyebrow(new QLabel(m_hero))
    , m_heading(new QLabel(m_hero))
    , m_heroText(new QLabel(m_hero))
    , m_steps(new VaultUnlockSteps(m_hero))
    , m_heroFoot(new QLabel(m_hero))
    , m_topRow(new QHBoxLayout())
    , m_contentLayout(new QVBoxLayout())
{
    setObjectName("vaultUnlockShell");

    m_hero->setObjectName("vaultUnlockHero");
    m_hero->setAttribute(Qt::WA_StyledBackground, true);
    auto* hero = new QVBoxLayout(m_hero);
    hero->setContentsMargins(56, 48, 56, 40);
    hero->setSpacing(0);

    auto* brand = new QHBoxLayout();
    brand->setSpacing(10);
    m_logo->setPixmap(icons()->applicationIcon().pixmap(28, 28));
    m_logo->setFixedSize(28, 28);
    brand->addWidget(m_logo);
    brand->addWidget(m_brand);
    brand->addStretch();
    hero->addLayout(brand);
    hero->addStretch(1);

    m_eyebrow->setObjectName("vaultUnlockEyebrow");
    hero->addWidget(m_eyebrow);
    hero->addSpacing(16);
    m_heading->setObjectName("vaultUnlockHeading");
    m_heading->setWordWrap(true);
    hero->addWidget(m_heading);
    hero->addSpacing(20);
    m_heroText->setWordWrap(true);
    hero->addWidget(m_heroText);
    hero->addSpacing(32);
    hero->addWidget(m_steps);
    hero->addStretch(1);
    m_heroFoot->setText(tr("Open source under GPLv3"));
    hero->addWidget(m_heroFoot);
    m_hero->setMaximumWidth(720);

    // The column: a top-right link, the form in the middle, the legal links at the bottom
    auto* column = new QVBoxLayout();
    column->setContentsMargins(40, 32, 40, 24);
    column->setSpacing(0);
    m_topRow->addStretch();
    column->addLayout(m_topRow);
    column->addStretch(2);
    auto* centre = new QHBoxLayout();
    centre->addStretch();
    centre->addLayout(m_contentLayout);
    centre->addStretch();
    column->addLayout(centre);
    column->addStretch(3);
    auto* legal = new QHBoxLayout();
    legal->setSpacing(24);
    legal->addStretch();
    const QList<QPair<QString, QString>> links = {{tr("Help"), Site + "/docs/"},
                                                  {tr("Contact"), Site + "/support/"},
                                                  {tr("Terms"), Site + "/terms/"},
                                                  {tr("Privacy"), Site + "/privacy/"}};
    for (const auto& link : links) {
        auto* label = new QLabel(this);
        label->setProperty("href", link.second);
        label->setProperty("linkText", link.first);
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        connect(label, &QLabel::linkActivated, this, [](const QString& url) { QDesktopServices::openUrl(QUrl(url)); });
        legal->addWidget(label);
        m_legalLinks.append(label);
    }
    legal->addStretch();
    column->addLayout(legal);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_hero, 21);
    layout->addLayout(column, 20);

    m_steps->setSteps({{tr("Enter your password."), tr("Only on this device.")},
                       {tr("Make your key."), tr("Your password is stretched into the key that opens your vault.")},
                       {tr("Open your vault."), tr("Decrypted here, never on a server.")}});

    Vault::followTheme(this, [this] { applyTheme(); });
}

void VaultUnlockShell::setHero(const QString& eyebrow, const QString& heading, const QString& text)
{
    m_eyebrow->setText(eyebrow.toUpper());
    m_heading->setText(heading);
    m_heroText->setText(text);
}

VaultUnlockSteps* VaultUnlockShell::steps() const
{
    return m_steps;
}

void VaultUnlockShell::setContent(QWidget* content)
{
    content->setParent(this);
    content->setMaximumWidth(440);
    content->setMinimumWidth(qMin(360, content->minimumWidth() > 0 ? content->minimumWidth() : 360));
    m_contentLayout->addWidget(content);
}

void VaultUnlockShell::setTopLink(QWidget* link)
{
    link->setParent(this);
    m_topRow->addWidget(link);
}

bool VaultUnlockShell::isHeroVisible() const
{
    return m_hero->isVisibleTo(this);
}

void VaultUnlockShell::resizeEvent(QResizeEvent* event)
{
    m_hero->setVisible(width() >= HeroBreakpoint);
    QWidget::resizeEvent(event);
}

void VaultUnlockShell::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter(this).fillRect(rect(), FluentTheme::palette().layer);
}

void VaultUnlockShell::applyTheme()
{
    const auto p = FluentTheme::palette();
    const auto c = heroColors();
    m_hero->setStyleSheet(QStringLiteral("QFrame#vaultUnlockHero { background-color: %1; }").arg(rgba(c.background)));

    const auto setColor = [](QLabel* label, const QColor& textColor) {
        auto palette = label->palette();
        palette.setColor(QPalette::WindowText, textColor);
        palette.setColor(QPalette::Text, textColor);
        label->setPalette(palette);
    };
    m_brand->setFont(Vault::headingFont(19));
    setColor(m_brand, c.text);

    QFont eyebrow(FluentTheme::monoFontFamily().isEmpty() ? QStringLiteral("monospace")
                                                          : FluentTheme::monoFontFamily());
    eyebrow.setPixelSize(13);
    eyebrow.setWeight(QFont::Medium);
    eyebrow.setLetterSpacing(QFont::PercentageSpacing, 104);
    m_eyebrow->setFont(eyebrow);
    setColor(m_eyebrow, c.eyebrow);

    auto heading = Vault::headingFont(52);
    heading.setLetterSpacing(QFont::PercentageSpacing, 96.5);
    m_heading->setFont(heading);
    setColor(m_heading, c.text);

    auto body = Vault::font(Typography::FontRole::BodyLarge);
    m_heroText->setFont(body);
    setColor(m_heroText, c.muted);

    m_heroFoot->setFont(Vault::font(Typography::FontRole::Caption));
    setColor(m_heroFoot, c.faint);

    for (auto* label : m_legalLinks) {
        label->setFont(Vault::font(Typography::FontRole::Caption));
        label->setText(
            linkHtml(label->property("href").toString(), label->property("linkText").toString(), p.textSecondary));
    }
    m_steps->applyTheme();
    update();
}
