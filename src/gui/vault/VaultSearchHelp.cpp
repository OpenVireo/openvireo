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

#include "VaultSearchHelp.h"

#include "gui/Font.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace
{
    QFont codeFont()
    {
        // Fluent fonts are sized in pixels
        auto font = Font::fixedFont();
        const auto caption = Vault::font(Typography::FontRole::Caption);
        if (caption.pixelSize() > 0) {
            font.setPixelSize(caption.pixelSize());
        } else {
            font.setPointSizeF(caption.pointSizeF());
        }
        return font;
    }

    QLabel* themedLabel(const QString& text, Typography::FontRole role, bool secondary, QWidget* parent)
    {
        auto* label = new QLabel(text, parent);
        Vault::followTheme(label, [label, role, secondary] {
            const auto p = FluentTheme::palette();
            Vault::styleLabel(label, role, secondary ? p.textSecondary : p.text);
        });
        return label;
    }

    // A piece of search syntax in a code pill, such as "user:"
    class TokenPill : public QAbstractButton
    {
    public:
        TokenPill(const QString& token, bool clickable, QWidget* parent)
            : QAbstractButton(parent)
        {
            setText(token);
            setAccessibleName(token);
            setFocusPolicy(clickable ? Qt::TabFocus : Qt::NoFocus);
            setAttribute(Qt::WA_TransparentForMouseEvents, !clickable);
            if (clickable) {
                setCursor(Qt::PointingHandCursor);
                setAttribute(Qt::WA_Hover);
            }
            setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        }

        QSize sizeHint() const override
        {
            return {QFontMetrics(codeFont()).horizontalAdvance(text()) + 14, 22};
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            QPainterPath path;
            path.addRoundedRect(r, 4, 4);
            painter.fillPath(path, isDown() ? p.controlPressed : underMouse() ? p.controlHover : p.control);
            painter.setPen(QPen(hasFocus() ? p.text : p.stroke, hasFocus() ? 2 : 1));
            painter.drawPath(path);
            painter.setFont(codeFont());
            painter.setPen(p.text);
            painter.drawText(r, Qt::AlignCenter, text());
        }
    };

    // An example query with what it finds; the whole row is clickable
    class ExampleRow : public QAbstractButton
    {
    public:
        ExampleRow(const QString& query, const QString& meaning, QWidget* parent)
            : QAbstractButton(parent)
            , m_meaning(meaning)
        {
            setText(query);
            setAccessibleName(query);
            setAccessibleDescription(meaning);
            setToolTip(VaultSearchHelp::tr("Search for %1").arg(query));
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::TabFocus);
            setAttribute(Qt::WA_Hover);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }

        QSize sizeHint() const override
        {
            const auto queryWidth = QFontMetrics(codeFont()).horizontalAdvance(text());
            const auto meaningWidth =
                QFontMetrics(Vault::font(Typography::FontRole::Caption)).horizontalAdvance(m_meaning);
            return {qMax(queryWidth, 190) + meaningWidth + 40, 30};
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            if (underMouse() || isDown() || hasFocus()) {
                QPainterPath path;
                path.addRoundedRect(r, 4, 4);
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
                if (hasFocus()) {
                    painter.setPen(QPen(p.text, 2));
                    painter.drawPath(path);
                }
            }

            const auto text = r.adjusted(8, 0, -8, 0);
            painter.setFont(codeFont());
            painter.setPen(p.accentText);
            painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, this->text());

            painter.setFont(Vault::font(Typography::FontRole::Caption));
            painter.setPen(p.textSecondary);
            painter.drawText(text, Qt::AlignRight | Qt::AlignVCenter, m_meaning);
        }

    private:
        QString m_meaning;
    };
} // namespace

VaultSearchHelp::VaultSearchHelp(QWidget* parent)
    : Flyout(parent)
{
    setObjectName("vaultSearchHelp");
    setPlacement(Auto);
    setAnchorOffset(6);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(contentsMargins() + QMargins(16, 14, 16, 12));
    layout->setSpacing(4);

    // Title with the shortcut that toggles the flyout
    auto* header = new QHBoxLayout();
    header->addWidget(themedLabel(tr("Search tips"), Typography::FontRole::BodyStrong, false, this));
    header->addStretch();
    header->addWidget(themedLabel(QKeySequence(Qt::CTRL | Qt::Key_J).toString(QKeySequence::NativeText),
                                  Typography::FontRole::Caption,
                                  true,
                                  this));
    layout->addLayout(header);
    auto* intro = themedLabel(
        tr("Every term must match. Quote terms that contain spaces."), Typography::FontRole::Caption, true, this);
    layout->addWidget(intro);

    // Fields, with their short forms in the tooltip
    addSection(layout, tr("Search in a field"));
    const QList<QPair<QString, QString>> fields = {{"title:", "t:"},
                                                   {"user:", "u:"},
                                                   {"url:", {}},
                                                   {"notes:", "n:"},
                                                   {"pw:", "p:, password:"},
                                                   {"tag:", "tags:"},
                                                   {"group:", "g:"},
                                                   {"attach:", "attachment:"},
                                                   {"attr:", "attribute:"},
                                                   {"uuid:", {}}};
    // Pills flow in rows of five, each row packed to the left
    QHBoxLayout* fieldRow = nullptr;
    for (int i = 0; i < fields.size(); ++i) {
        const auto& field = fields.at(i);
        auto* pill = new TokenPill(field.first, true, this);
        pill->setToolTip(field.second.isEmpty() ? tr("Insert %1").arg(field.first)
                                                : tr("Insert %1 (also %2)").arg(field.first, field.second));
        connect(pill, &QAbstractButton::clicked, this, [this, field] { emit insertRequested(field.first); });
        if (i % 5 == 0) {
            if (fieldRow) {
                fieldRow->addStretch();
            }
            fieldRow = new QHBoxLayout();
            fieldRow->setSpacing(6);
            layout->addLayout(fieldRow);
            layout->addSpacing(2);
        }
        fieldRow->addWidget(pill);
    }
    fieldRow->addStretch();
    layout->addSpacing(2);
    layout->addWidget(themedLabel(tr("Use _name: for a custom attribute."), Typography::FontRole::Caption, true, this));

    // Modifiers go before a term, wildcards inside it
    addSection(layout, tr("Refine a term"));
    auto* syntax = new QGridLayout();
    syntax->setHorizontalSpacing(8);
    syntax->setVerticalSpacing(6);
    addSyntaxRow(syntax, 0, 0, "-", tr("exclude"));
    addSyntaxRow(syntax, 1, 0, "+", tr("exact match"));
    addSyntaxRow(syntax, 2, 0, "*", tr("regular expression"));
    addSyntaxRow(syntax, 0, 3, "*", tr("any characters"));
    addSyntaxRow(syntax, 1, 3, "?", tr("one character"));
    addSyntaxRow(syntax, 2, 3, "|", tr("either side"));
    syntax->setColumnMinimumWidth(2, 16);
    syntax->setColumnStretch(5, 1);
    layout->addLayout(syntax);
    layout->addWidget(themedLabel(
        tr("Put -, + or * before a term, and wildcards inside it."), Typography::FontRole::Caption, true, this));

    addSection(layout, tr("Try an example"));
    const QList<QPair<QString, QString>> examples = {
        {"user:alice url:github", tr("Alice's logins on GitHub")},
        {"tag:work -group:archive", tr("Work entries outside Archive")},
        {"title:\"bank|card\"", tr("Titles with bank or card")},
        {"*notes:\"pin \\d{4}\"", tr("Four-digit PINs in notes")},
    };
    for (const auto& example : examples) {
        auto* row = new ExampleRow(example.first, example.second, this);
        connect(row, &QAbstractButton::clicked, this, [this, example] {
            emit searchRequested(example.first);
            close();
        });
        layout->addWidget(row);
    }

    Vault::followTheme(this, [this] {
        for (auto* child : findChildren<QAbstractButton*>()) {
            child->update();
        }
    });
}

void VaultSearchHelp::addSection(QVBoxLayout* layout, const QString& title)
{
    layout->addSpacing(10);
    layout->addWidget(themedLabel(title, Typography::FontRole::Caption, false, this));
    layout->addSpacing(2);
}

void VaultSearchHelp::addSyntaxRow(QGridLayout* grid, int row, int column, const QString& token, const QString& meaning)
{
    grid->addWidget(new TokenPill(token, false, this), row, column, Qt::AlignLeft);
    grid->addWidget(themedLabel(meaning, Typography::FontRole::Caption, true, this), row, column + 1);
}
