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

#include "EntryDetailCard.h"

#include "core/Clock.h"
#include "core/Config.h"
#include "core/Entry.h"
#include "core/EntryAttachments.h"
#include "core/Group.h"
#include "core/Totp.h"
#include "gui/Font.h"
#include "gui/Icons.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    constexpr int TileSize = 44;

    const QString HiddenText = QStringLiteral("••••••••••••");

    // Rounded surface holding the field rows, one step quieter than the card
    class FieldGroup : public QWidget
    {
    public:
        using QWidget::QWidget;

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            painter.fillPath(path, p.inset);
            painter.setPen(QPen(p.stroke, 1));
            painter.drawPath(path);

            // Dividers between visible rows
            painter.setPen(QPen(p.divider, 1));
            QWidget* previous = nullptr;
            for (auto* child : findChildren<VaultFieldRow*>(Qt::FindDirectChildrenOnly)) {
                if (!child->isVisible()) {
                    continue;
                }
                if (previous) {
                    painter.drawLine(QPointF(1, child->y() + 0.5), QPointF(width() - 1, child->y() + 0.5));
                }
                previous = child;
            }
        }
    };

    QString resolved(const Entry* entry, const QString& value)
    {
        return entry->resolveMultiplePlaceholders(value);
    }

    QString formatTotp(const QString& code)
    {
        // "482913" reads better as "482 913"
        if (code.size() == 6 || code.size() == 8) {
            const int half = code.size() / 2;
            return code.left(half) + QLatin1Char(' ') + code.mid(half);
        }
        return code;
    }
} // namespace

EntryDetailCard::EntryDetailCard(QWidget* parent)
    : fluent::layout::Card(parent)
    , m_pages(new QStackedLayout())
    , m_emptyTitle(new QLabel(this))
    , m_emptyText(new QLabel(this))
    , m_icon(new QLabel(this))
    , m_title(new QLabel(this))
    , m_subtitle(new QLabel(this))
    , m_meta(new QLabel(this))
    , m_notesLabel(new QLabel(tr("Notes"), this))
    , m_notes(new QLabel(this))
    , m_totpTimer(new QTimer(this))
{
    setObjectName("entryDetailCard");
    setAppearance(fluent::layout::Card::Layer);
    setBorderVisible(true);
    setMinimumWidth(320);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addLayout(m_pages);

    // Empty state, shown when a group is selected but no entry
    auto* emptyPage = new QWidget(this);
    auto* emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->addStretch();
    m_emptyTitle->setAlignment(Qt::AlignCenter);
    m_emptyTitle->setWordWrap(true);
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setWordWrap(true);
    emptyLayout->addWidget(m_emptyTitle);
    emptyLayout->addWidget(m_emptyText);
    emptyLayout->addStretch();
    m_pages->addWidget(emptyPage);

    // Entry details, scrollable for long notes
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    // After setWidget(), which turns the fill back on and would paint the window color over the card
    content->setAutoFillBackground(false);
    m_pages->addWidget(scroll);

    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    // Header: icon, title, group and modified time, Auto-Type and Edit
    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    m_icon->setFixedSize(TileSize, TileSize);
    m_icon->setAlignment(Qt::AlignCenter);
    header->addWidget(m_icon);
    auto* titles = new QVBoxLayout();
    titles->setSpacing(0);
    m_title->setWordWrap(true);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);
    header->addLayout(titles, 1);

    m_autoTypeButton = new fluent::basicinput::Button(tr("Auto-Type"), content);
    m_autoTypeButton->setFluentLayout(fluent::basicinput::Button::IconBefore);
    m_autoTypeButton->setFluentSize(fluent::basicinput::Button::Small);
    m_autoTypeButton->setIconGlyph(Typography::Icons::Keyboard);
    m_autoTypeButton->setToolTip(tr("Perform Auto-Type"));
    Vault::bindAction(m_autoTypeButton, "actionEntryAutoTypeSequence");
    header->addWidget(m_autoTypeButton, 0, Qt::AlignTop);

    m_editButton = Vault::iconButton(Typography::Icons::Edit, tr("Edit entry"), content);
    m_editButton->setFluentStyle(fluent::basicinput::Button::Standard);
    m_editButton->setFluentSize(fluent::basicinput::Button::Small);
    Vault::bindAction(m_editButton, "actionEntryEdit");
    header->addWidget(m_editButton, 0, Qt::AlignTop);
    layout->addLayout(header);

    // Fields
    m_fields = new FieldGroup(content);
    auto* fieldsLayout = new QVBoxLayout(m_fields);
    fieldsLayout->setContentsMargins(0, 0, 0, 0);
    fieldsLayout->setSpacing(0);

    m_usernameRow = new VaultFieldRow(tr("Username"), m_fields);
    Vault::bindAction(m_usernameRow->addButton(Typography::Icons::Copy, tr("Copy username")),
                      "actionEntryCopyUsername");
    m_usernameRevealButton = m_usernameRow->addButton(Typography::Icons::RevealPasswordMedium, tr("Show username"));
    connect(m_usernameRevealButton, &QAbstractButton::clicked, this, [this] {
        m_usernameRevealed = !m_usernameRevealed;
        updateFields();
    });
    fieldsLayout->addWidget(m_usernameRow);

    m_passwordRow = new VaultFieldRow(tr("Password"), m_fields);
    Vault::bindAction(m_passwordRow->addButton(Typography::Icons::Copy, tr("Copy password")),
                      "actionEntryCopyPassword");
    m_passwordRevealButton = m_passwordRow->addButton(Typography::Icons::RevealPasswordMedium, tr("Show password"));
    connect(m_passwordRevealButton, &QAbstractButton::clicked, this, [this] {
        m_passwordRevealed = !m_passwordRevealed;
        updateFields();
    });
    fieldsLayout->addWidget(m_passwordRow);

    m_totpRow = new VaultFieldRow(tr("One-time code"), m_fields);
    Vault::bindAction(m_totpRow->addButton(Typography::Icons::Copy, tr("Copy TOTP")), "actionEntryCopyTotp");
    m_totpRevealButton = m_totpRow->addButton(Typography::Icons::RevealPasswordMedium, tr("Show TOTP"));
    connect(m_totpRevealButton, &QAbstractButton::clicked, this, [this] {
        m_totpRevealed = !m_totpRevealed;
        updateTotp();
    });
    fieldsLayout->addWidget(m_totpRow);

    m_urlRow = new VaultFieldRow(tr("URL"), m_fields);
    Vault::bindAction(m_urlRow->addButton(Typography::Icons::Link, tr("Open URL")), "actionEntryOpenUrl");
    fieldsLayout->addWidget(m_urlRow);
    layout->addWidget(m_fields);

    // Tags, expiry and attachments
    m_meta->setWordWrap(true);
    m_meta->setTextFormat(Qt::PlainText);
    layout->addWidget(m_meta);

    // Notes
    auto* notesHeader = new QHBoxLayout();
    notesHeader->addWidget(m_notesLabel);
    notesHeader->addStretch();
    m_notesRevealButton = Vault::iconButton(Typography::Icons::RevealPasswordMedium, tr("Show notes"), content);
    m_notesRevealButton->setFluentSize(fluent::basicinput::Button::Small);
    connect(m_notesRevealButton, &QAbstractButton::clicked, this, [this] {
        m_notesRevealed = !m_notesRevealed;
        updateMeta();
    });
    notesHeader->addWidget(m_notesRevealButton);
    layout->addLayout(notesHeader);
    m_notes->setWordWrap(true);
    m_notes->setTextFormat(Qt::PlainText);
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_notes->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(m_notes);
    layout->addStretch();

    m_totpTimer->setInterval(1000);
    connect(m_totpTimer, &QTimer::timeout, this, &EntryDetailCard::updateTotp);

    applyColors();
    setGroup(nullptr);
}

void EntryDetailCard::setEntry(Entry* entry)
{
    if (!entry) {
        setGroup(m_group);
        return;
    }
    if (entry != m_entry) {
        if (m_entry) {
            disconnect(m_entry, nullptr, this, nullptr);
        }
        connect(entry, &Entry::modified, this, &EntryDetailCard::refresh);
        m_usernameRevealed = false;
        m_passwordRevealed = false;
        m_totpRevealed = false;
        m_notesRevealed = false;
    }
    m_entry = entry;
    refresh();
}

void EntryDetailCard::setGroup(Group* group)
{
    if (m_entry) {
        disconnect(m_entry, nullptr, this, nullptr);
    }
    m_entry = nullptr;
    m_group = group;
    m_totpTimer->stop();

    if (group) {
        m_emptyTitle->setText(group->name());
        const int count = group->entries().size();
        m_emptyText->setText(count == 0 ? tr("This group has no entries yet.")
                                        : tr("Select an entry to see its details."));
    } else {
        m_emptyTitle->setText(tr("No entry selected"));
        m_emptyText->setText(tr("Select an entry to see its details."));
    }
    m_pages->setCurrentIndex(0);
}

void EntryDetailCard::refresh()
{
    if (!m_entry) {
        setGroup(m_group);
        return;
    }
    updateHeader();
    updateFields();
    updateTotp();
    updateMeta();
    m_pages->setCurrentIndex(1);
}

void EntryDetailCard::onThemeUpdated()
{
    fluent::layout::Card::onThemeUpdated();
    applyColors();
}

void EntryDetailCard::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_emptyTitle, Typography::FontRole::Subtitle, p.text);
    Vault::styleLabel(m_emptyText, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_title, Typography::FontRole::Subtitle, p.text);
    Vault::styleLabel(m_subtitle, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_meta, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_notesLabel, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_notes, Typography::FontRole::Body, p.textSecondary);
    for (auto* row : {m_usernameRow, m_passwordRow, m_totpRow, m_urlRow}) {
        row->applyColors();
    }
    Vault::styleLabel(m_urlRow->value(), Typography::FontRole::Body, p.accentText);
    if (m_entry) {
        updateHeader();
        updateMeta();
    }
}

void EntryDetailCard::updateHeader()
{
    const auto p = FluentTheme::palette();

    // Entry icon on a rounded tile
    QPixmap tile(QSize(TileSize, TileSize) * devicePixelRatioF());
    tile.setDevicePixelRatio(devicePixelRatioF());
    tile.fill(Qt::transparent);
    {
        QPainter painter(&tile);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(0.5, 0.5, TileSize - 1, TileSize - 1), 8, 8);
        painter.fillPath(path, p.inset);
        painter.setPen(QPen(p.stroke, 1));
        painter.drawPath(path);
        const auto icon = Icons::entryIconPixmap(m_entry, IconSize::Medium);
        const int size = 24;
        painter.drawPixmap(QRect((TileSize - size) / 2, (TileSize - size) / 2, size, size), icon);
    }
    m_icon->setPixmap(tile);

    m_title->setText(m_entry->resolveMultiplePlaceholders(m_entry->title()));
    QStringList subtitle;
    if (m_entry->group()) {
        subtitle << m_entry->group()->name();
    }
    const auto modified = m_entry->timeInfo().lastModificationTime().toLocalTime();
    subtitle << tr("modified %1").arg(QLocale().toString(modified, QLocale::ShortFormat));
    m_subtitle->setText(subtitle.join(QStringLiteral(" · ")));
}

void EntryDetailCard::updateFields()
{
    const auto username = resolved(m_entry, m_entry->username());
    const bool hideUsername = config()->get(Config::GUI_HideUsernames).toBool();
    m_usernameRow->setVisible(!username.isEmpty());
    m_usernameRow->value()->setText(hideUsername && !m_usernameRevealed ? HiddenText : username);
    m_usernameRevealButton->setVisible(hideUsername);
    m_usernameRevealButton->setToolTip(m_usernameRevealed ? tr("Hide username") : tr("Show username"));

    const auto password = resolved(m_entry, m_entry->password());
    const bool hidePassword = config()->get(Config::Security_HidePasswordPreviewPanel).toBool();
    m_passwordRow->setVisible(!password.isEmpty());
    m_passwordRow->value()->setText(hidePassword && !m_passwordRevealed ? HiddenText : password);
    m_passwordRevealButton->setVisible(hidePassword);
    m_passwordRevealButton->setToolTip(m_passwordRevealed ? tr("Hide password") : tr("Show password"));

    const auto url = resolved(m_entry, m_entry->displayUrl());
    m_urlRow->setVisible(!url.isEmpty());
    m_urlRow->value()->setText(url);
    m_urlRow->value()->setToolTip(url);

    m_fields->setVisible(!username.isEmpty() || !password.isEmpty() || !url.isEmpty() || m_entry->hasTotp());
    m_fields->update();
}

void EntryDetailCard::updateTotp()
{
    if (!m_entry || !m_entry->hasTotp()) {
        m_totpRow->setVisible(false);
        m_totpTimer->stop();
        return;
    }

    m_totpRow->setVisible(true);
    const bool hideTotp = config()->get(Config::Security_HideTotpPreviewPanel).toBool();
    m_totpRevealButton->setVisible(hideTotp);
    m_totpRevealButton->setToolTip(m_totpRevealed ? tr("Hide TOTP") : tr("Show TOTP"));

    if (hideTotp && !m_totpRevealed) {
        m_totpRow->value()->setText(HiddenText);
        m_totpTimer->stop();
        return;
    }

    const auto step = m_entry->totpSettings()->step;
    const auto remaining = step - (Clock::currentSecondsSinceEpoch() % step);
    m_totpRow->value()->setText(
        tr("%1   %2 s left").arg(formatTotp(m_entry->totp()), QString::number(remaining)));
    if (!m_totpTimer->isActive()) {
        m_totpTimer->start();
    }
}

void EntryDetailCard::updateMeta()
{
    QStringList meta;
    const auto tags = m_entry->tagList();
    if (!tags.isEmpty()) {
        meta << tr("Tags: %1").arg(tags.join(QStringLiteral(", ")));
    }
    const auto& timeInfo = m_entry->timeInfo();
    if (timeInfo.expires()) {
        const auto expiry = QLocale().toString(timeInfo.expiryTime().toLocalTime(), QLocale::ShortFormat);
        meta << (m_entry->isExpired() ? tr("Expired %1").arg(expiry) : tr("Expires %1").arg(expiry));
    } else {
        meta << tr("Expires never");
    }
    const auto attachments = m_entry->attachments()->keys();
    if (attachments.size() == 1) {
        meta << tr("Attachment: %1").arg(attachments.first());
    } else if (attachments.size() > 1) {
        meta << tr("%n attachment(s)", "", attachments.size());
    }
    m_meta->setText(meta.join(QStringLiteral("  ·  ")));

    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_meta, Typography::FontRole::Caption, m_entry->isExpired() ? p.critical : p.textSecondary);

    const auto notes = m_entry->resolveMultiplePlaceholders(m_entry->notes());
    const bool hideNotes = config()->get(Config::Security_HideNotes).toBool();
    const bool hasNotes = !notes.isEmpty();
    m_notesLabel->setVisible(hasNotes);
    m_notes->setVisible(hasNotes);
    m_notesRevealButton->setVisible(hasNotes && hideNotes);
    m_notesRevealButton->setToolTip(m_notesRevealed ? tr("Hide notes") : tr("Show notes"));
    m_notes->setText(hideNotes && !m_notesRevealed ? HiddenText : notes);
    m_notes->setFont(config()->get(Config::GUI_MonospaceNotes).toBool() ? Font::fixedFont()
                                                                        : Vault::font(Typography::FontRole::Body));
}
