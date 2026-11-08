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

#include "OrgAdminWidgets.h"

#include "gui/account/ServiceAccountsDialog.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <components/basicinput/Button.h>
#include <components/dialogs_flyouts/ContentDialog.h>
#include <components/textfields/Label.h>
#include <design/IconCatalog.h>

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace
{
    constexpr int RowHeight = 40;
    constexpr int RowHeightTwoLines = 54;
    constexpr int AvatarSize = 32;
    constexpr int NavigationRowHeight = 36;
    // Width of a prompt's card, as wide as WinUI's content dialogs with a form
    constexpr int PromptWidth = 480;

    struct BadgeColors
    {
        QColor background;
        QColor text;
    };

    BadgeColors badgeColors(OrgAdmin::BadgeKind kind)
    {
        const auto p = FluentTheme::palette();
        switch (kind) {
        case OrgAdmin::BadgeKind::Accent:
            return {p.accentSubtle, p.accentText};
        case OrgAdmin::BadgeKind::Caution:
            return {p.cautionBg, p.caution};
        case OrgAdmin::BadgeKind::Success:
            return {p.successBg, p.success};
        case OrgAdmin::BadgeKind::Critical:
            return {p.criticalBg, p.critical};
        case OrgAdmin::BadgeKind::Neutral:
            break;
        }
        return {p.inset, p.textSecondary};
    }

    // A pill with the text in it, ending at `right`; returns where it starts
    int paintBadge(QPainter* painter, const QString& text, OrgAdmin::BadgeKind kind, int right, int centerY)
    {
        if (text.isEmpty()) {
            return right;
        }
        const auto font = Vault::font(Typography::FontRole::Caption);
        const QFontMetrics metrics(font);
        const int width = metrics.horizontalAdvance(text) + 16;
        const int height = metrics.height() + 6;
        const QRectF pill(right - width, centerY - height / 2.0, width, height);
        const auto colors = badgeColors(kind);
        painter->setPen(Qt::NoPen);
        painter->setBrush(colors.background);
        painter->drawRoundedRect(pill, height / 2.0, height / 2.0);
        painter->setFont(font);
        painter->setPen(colors.text);
        painter->drawText(pill, Qt::AlignCenter, text);
        return right - width;
    }

    QString initials(const QString& text)
    {
        // Emails give the letter before the @; names give the first letters of the first two words
        const auto name = text.section('@', 0, 0);
        const auto words = name.split(QRegularExpression("[\\s._-]+"), Qt::SkipEmptyParts);
        QString letters;
        for (const auto& word : words.mid(0, 2)) {
            letters += word.at(0).toUpper();
        }
        return letters.isEmpty() ? QStringLiteral("?") : letters;
    }

    void paintRowBackground(QPainter* painter, const QStyleOptionViewItem& option, const QRectF& box)
    {
        const auto p = FluentTheme::palette();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? p.subtlePressed : p.subtleHover);
            painter->drawRoundedRect(box, 6, 6);
        }
        if (selected) {
            // WinUI's selection indicator: a short accent pill at the start of the row
            const QRectF pill(box.left() + 1, box.center().y() - 8, 3, 16);
            painter->setBrush(p.accent);
            painter->drawRoundedRect(pill, 1.5, 1.5);
        }
        const bool focused = (option.state & QStyle::State_HasFocus) && option.widget && option.widget->hasFocus();
        if (focused) {
            painter->setPen(QPen(p.accent, 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(box.adjusted(1, 1, -1, -1), 5, 5);
        }
    }
} // namespace

QLabel* OrgAdmin::label(QWidget* parent, Typography::FontRole role, bool secondary)
{
    // A plain label with its color in a style sheet: a card over the window (VaultModal) gives everything inside
    // it a new palette, which would wipe out a color kept in the palette, as Fluent-Qt's labels keep theirs
    auto label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    Vault::followTheme(label, [label, role, secondary] {
        const auto p = FluentTheme::palette();
        label->setFont(Vault::font(role));
        label->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }")
                                 .arg((secondary ? p.textSecondary : p.text).name(QColor::HexArgb)));
    });
    return label;
}

QWidget* OrgAdmin::pageHeader(const QString& title, const QString& description, QWidget* action, QWidget* parent)
{
    auto header = new QWidget(parent);
    auto layout = new QVBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto row = new QHBoxLayout();
    row->setSpacing(12);
    auto heading = label(header, Typography::FontRole::Subtitle);
    heading->setText(title);
    heading->setAccessibleName(title);
    row->addWidget(heading, 1);
    if (action) {
        row->addWidget(action, 0, Qt::AlignTop);
    }
    layout->addLayout(row);
    if (!description.isEmpty()) {
        auto text = label(header, Typography::FontRole::Body, true);
        text->setText(description);
        layout->addWidget(text);
    }
    return header;
}

QString OrgAdmin::listStyleSheet()
{
    // The rows paint themselves; the view only drops its frame, fill and stock selection
    return QStringLiteral("QAbstractItemView { background: transparent; border: none; outline: none; }"
                          "QAbstractItemView::item, QAbstractItemView::item:selected,"
                          " QAbstractItemView::item:hover { background: transparent; border: none; }"
                          "QTreeView::branch { background: transparent; }");
}

QString OrgAdmin::tableStyleSheet()
{
    const auto p = FluentTheme::palette();
    const auto argb = [](const QColor& color) { return color.name(QColor::HexArgb); };
    return QStringLiteral("QTreeView { background-color: %1; border: 1px solid %2; border-radius: 8px;"
                          " outline: none; padding: 2px; }"
                          "QTreeView::item { padding: 6px 4px; border: none; color: %3; }"
                          "QTreeView::item:hover { background-color: %4; }"
                          "QTreeView::item:selected { background-color: %5; color: %3; }"
                          "QTreeView::branch { background: transparent; }"
                          "QHeaderView { background: transparent; border: none; }"
                          "QHeaderView::section { background: transparent; border: none;"
                          " border-bottom: 1px solid %6; padding: 6px 8px; color: %7; }")
        .arg(argb(p.layer),
             argb(p.stroke),
             argb(p.text),
             argb(p.subtleHover),
             argb(p.subtlePressed),
             argb(p.divider),
             argb(p.textSecondary));
}

QSize OrgAdmin::RowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const bool twoLines = !index.data(SecondaryRole).toString().isEmpty() || index.data(AvatarRole).toBool();
    return {option.rect.width(), twoLines ? RowHeightTwoLines : RowHeight};
}

void OrgAdmin::RowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const auto p = FluentTheme::palette();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(option.rect).adjusted(2, 2, -2, -2);
    paintRowBackground(painter, option, box);

    const auto text = index.data(Qt::DisplayRole).toString();
    const auto secondary = index.data(SecondaryRole).toString();
    const int centerY = qRound(box.center().y());
    int left = qRound(box.left()) + 14;
    if (index.data(AvatarRole).toBool()) {
        const QRectF avatar(left, centerY - AvatarSize / 2.0, AvatarSize, AvatarSize);
        painter->setPen(Qt::NoPen);
        painter->setBrush(p.accentSubtle);
        painter->drawEllipse(avatar);
        painter->setFont(Vault::font(Typography::FontRole::BodyStrong));
        painter->setPen(p.accentText);
        painter->drawText(avatar, Qt::AlignCenter, initials(text));
        left += AvatarSize + 12;
    }

    const auto badge = index.data(BadgeRole).toString();
    const auto kind = static_cast<BadgeKind>(index.data(BadgeKindRole).toInt());
    const int badgeLeft = paintBadge(painter, badge, kind, qRound(box.right()) - 12, centerY);
    const int textRight = badge.isEmpty() ? qRound(box.right()) - 12 : badgeLeft - 12;
    const int width = qMax(0, textRight - left);

    const auto bodyFont = Vault::font(Typography::FontRole::Body);
    const auto captionFont = Vault::font(Typography::FontRole::Caption);
    const QFontMetrics body(bodyFont);
    const QFontMetrics caption(captionFont);
    painter->setFont(bodyFont);
    painter->setPen(p.text);
    if (secondary.isEmpty()) {
        painter->drawText(QRect(left, option.rect.top(), width, option.rect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          body.elidedText(text, Qt::ElideRight, width));
    } else {
        const int total = body.height() + 2 + caption.height();
        const int top = centerY - total / 2;
        painter->drawText(QRect(left, top, width, body.height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          body.elidedText(text, Qt::ElideRight, width));
        painter->setFont(captionFont);
        painter->setPen(p.textSecondary);
        painter->drawText(QRect(left, top + body.height() + 2, width, caption.height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          caption.elidedText(secondary, Qt::ElideRight, width));
    }
    painter->restore();
}

QSize OrgAdmin::NavigationDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    Q_UNUSED(index)
    return {option.rect.width(), NavigationRowHeight};
}

void OrgAdmin::NavigationDelegate::paint(QPainter* painter,
                                         const QStyleOptionViewItem& option,
                                         const QModelIndex& index) const
{
    const auto p = FluentTheme::palette();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(option.rect).adjusted(0, 1, 0, -1);
    paintRowBackground(painter, option, box);

    const bool selected = option.state & QStyle::State_Selected;
    const QRectF glyphRect(box.left() + 14, box.center().y() - 9, 18, 18);
    painter->setPen(selected ? p.accentText : p.text);
    Typography::Icons::paintGlyph(*painter, glyphRect, index.data(GlyphRole).toString(), 16);

    const int count = index.data(CountRole).toInt();
    const int right = qRound(box.right()) - 10;
    const int badgeLeft =
        count > 0 ? paintBadge(painter, QString::number(count), BadgeKind::Caution, right, qRound(box.center().y()))
                  : right;
    painter->setFont(Vault::font(selected ? Typography::FontRole::BodyStrong : Typography::FontRole::Body));
    painter->setPen(p.text);
    const QRect textRect(qRound(glyphRect.right()) + 12,
                         option.rect.top(),
                         badgeLeft - 8 - (qRound(glyphRect.right()) + 12),
                         option.rect.height());
    painter->drawText(
        textRect,
        Qt::AlignVCenter | Qt::AlignLeft,
        painter->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width()));
    painter->restore();
}

bool OrgAdmin::Prompt::canShowFluent(QWidget* parent)
{
    return FluentTheme::vaultLayoutEnabled() && parent && parent->window()->isVisible()
           && !parent->window()->isMinimized();
}

OrgAdmin::Prompt::Prompt(QWidget* parent, const QString& title, const QString& acceptText, const QString& objectName)
    : m_fluent(canShowFluent(parent))
{
    m_content = new QWidget();
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(12);

    if (m_fluent) {
        using fluent::dialogs_flyouts::ContentDialog;
        auto dialog = new ContentDialog(parent->window());
        dialog->setThemeSource(parent->window());
        dialog->setTitle(title);
        dialog->setPrimaryButtonText(acceptText);
        dialog->setCloseButtonText(QObject::tr("Cancel"));
        dialog->setDefaultButton(ContentDialog::Primary);
        for (auto button : dialog->findChildren<fluent::basicinput::Button*>()) {
            if (button->text() == acceptText) {
                m_accept = button;
            }
        }
        m_dialog = dialog;
    } else {
        m_dialog = new QDialog(parent);
        m_dialog->setWindowTitle(title);
        auto layout = new QVBoxLayout(m_dialog);
        layout->addWidget(m_content);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, m_dialog);
        m_accept = buttons->addButton(acceptText, QDialogButtonBox::AcceptRole);
        m_accept->setDefault(true);
        QObject::connect(buttons, &QDialogButtonBox::accepted, m_dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, m_dialog, &QDialog::reject);
        layout->addWidget(buttons);
        m_content->setMinimumWidth(PromptWidth - 48);
    }
    m_dialog->setObjectName(objectName);
}

OrgAdmin::Prompt::~Prompt()
{
    // The form belongs to the dialog once it was shown
    if (!m_content->parent()) {
        delete m_content;
    }
    delete m_dialog;
}

QWidget* OrgAdmin::Prompt::content() const
{
    return m_content;
}

QVBoxLayout* OrgAdmin::Prompt::layout() const
{
    return m_layout;
}

QLabel* OrgAdmin::Prompt::addText(const QString& text)
{
    auto paragraph = label(m_content, Typography::FontRole::Body);
    paragraph->setText(text);
    m_layout->addWidget(paragraph);
    return paragraph;
}

QPushButton* OrgAdmin::Prompt::acceptButton() const
{
    return m_accept;
}

bool OrgAdmin::Prompt::exec()
{
    if (m_fluent) {
        auto dialog = static_cast<fluent::dialogs_flyouts::ContentDialog*>(m_dialog);
        // Wrapped only now that the form is complete: the wrapper lays the content out as it takes it, and the
        // text wraps, so the content asks for its height at the width the dialog gives it
        dialog->setContent(new VaultFittedContent(m_content));
        dialog->resize(PromptWidth + 2 * dialog->shadowSize(), dialog->minimumHeight());
        return dialog->exec() == fluent::dialogs_flyouts::ContentDialog::ResultPrimary;
    }
    return m_dialog->exec() == QDialog::Accepted;
}

OrgAdminModal::OrgAdminModal(QWidget* window, ServiceAccountsDialog* dialog)
    : VaultModal(window, dialog, ServiceAccountsDialog::tr("Organization"), QStringLiteral("orgAdmin"))
    , m_dialog(dialog)
{
    setMaximumCardWidth(1120);
    dialog->setEmbedded(true);
    auto done = addFooterButton(tr("Done"), true, QStringLiteral("Done"));
    connect(done, &QPushButton::clicked, this, &OrgAdminModal::reject);
    // Closing the window's content, such as with Escape, closes the card with it
    connect(dialog, &QDialog::finished, this, [this] {
        close();
        deleteLater();
    });
    setInitialFocus(dialog->navigation());
}

ServiceAccountsDialog* OrgAdminModal::dialog() const
{
    return m_dialog;
}

void OrgAdminModal::accept()
{
}

void OrgAdminModal::reject()
{
    if (m_dialog) {
        m_dialog->reject();
    } else {
        close();
        deleteLater();
    }
}
