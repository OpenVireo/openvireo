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

#include "VaultEntryEditCard.h"

#include "core/Entry.h"
#include "core/EntryAttachments.h"
#include "core/EntryAttributes.h"
#include "core/Totp.h"
#include "gui/CategoryListWidget.h"
#include "gui/DatabaseIcons.h"
#include "gui/EditWidget.h"
#include "gui/EditWidgetIcons.h"
#include "gui/Font.h"
#include "gui/Icons.h"
#include "gui/entry/EditEntryWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/tag/TagsEdit.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultEditShell.h"
#include "gui/vault/VaultMetrics.h"
#include "gui/vault/VaultWidgets.h"

#include <components/textfields/LineEdit.h>

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    constexpr int TileSize = 44;
    constexpr int SectionRowHeight = 52;
    constexpr int GroupGap = 14;
    constexpr int HeadingGap = 8;
    constexpr int CaptionWidth = 112;
    constexpr int NotesMinHeight = 96;
    constexpr int NotesMaxHeight = 200;
    // Below this width the captions go above their fields, as on the detail card in a narrow window
    constexpr int StackedBelow = 420;

    // Properties on the fields of rows that are built again whenever the attributes change
    constexpr const char* KeyProperty = "vaultEditKey";
    constexpr const char* PartProperty = "vaultEditPart";

    enum ExpiryChoice
    {
        ExpiresNever,
        ExpiresIn3Months,
        ExpiresIn1Year,
        ExpiresOnDate,
    };

    // The entry's icon on a rounded tile, as on the detail card; it opens the Icon page
    class IconTile : public QAbstractButton
    {
    public:
        explicit IconTile(QWidget* parent)
            : QAbstractButton(parent)
        {
            setFixedSize(TileSize, TileSize);
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
            setAttribute(Qt::WA_Hover);
        }

        void setPixmap(const QPixmap& pixmap)
        {
            m_pixmap = pixmap;
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
            painter.fillPath(path, p.layer);
            if (isEnabled() && (isDown() || underMouse())) {
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
            }
            // Dashed while editable, as a hint that it can be changed
            QPen pen(hasFocus() ? p.text : p.strokeStrong, hasFocus() ? 2 : 1);
            if (isEnabled() && !hasFocus()) {
                pen.setStyle(Qt::DashLine);
            }
            painter.setPen(pen);
            painter.drawPath(path);
            if (!m_pixmap.isNull()) {
                const int size = 24;
                painter.drawPixmap(QRect((width() - size) / 2, (height() - size) / 2, size, size), m_pixmap);
            }
        }

    private:
        QPixmap m_pixmap;
    };

    // Rounded surface one step quieter than the card, holding rows, as the detail card groups its fields
    class InsetGroup : public QWidget
    {
    public:
        InsetGroup(QWidget* parent, bool dividers)
            : QWidget(parent)
            , m_dividers(dividers)
        {
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                VaultMetrics::PanelRadius,
                                VaultMetrics::PanelRadius);
            painter.fillPath(path, p.inset);
            painter.setPen(QPen(p.stroke, 1));
            painter.drawPath(path);
            if (!m_dividers) {
                return;
            }
            // Dividers between visible rows
            painter.setPen(QPen(p.stroke, 1));
            bool first = true;
            for (auto* child : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                if (!child->isVisible()) {
                    continue;
                }
                if (!first) {
                    painter.drawLine(QPointF(1, child->y() + 0.5), QPointF(width() - 1, child->y() + 0.5));
                }
                first = false;
            }
        }

    private:
        bool m_dividers;
    };

    // One field: its caption on the left, the field (and anything under it) beside it, buttons on the right.
    // Stacked, the caption goes above.
    class EditRow : public QWidget
    {
    public:
        EditRow(QWidget* caption, QWidget* parent)
            : QWidget(parent)
            , m_caption(caption)
            , m_body(new QVBoxLayout())
            , m_actions(new QHBoxLayout())
            , m_grid(new QGridLayout(this))
        {
            setMinimumHeight(52);
            m_caption->setParent(this);
            m_body->setContentsMargins(0, 0, 0, 0);
            m_body->setSpacing(4);
            m_actions->setContentsMargins(0, 0, 0, 0);
            m_actions->setSpacing(2);
            m_grid->setHorizontalSpacing(12);
            m_grid->setVerticalSpacing(4);
            place();
        }

        QWidget* caption() const
        {
            return m_caption;
        }

        void addBody(QWidget* widget)
        {
            m_body->addWidget(widget);
        }

        void addBodyLayout(QLayout* layout)
        {
            m_body->addLayout(layout);
        }

        void addAction(QWidget* widget)
        {
            m_actions->addWidget(widget);
        }

        void setStacked(bool stacked)
        {
            if (stacked != m_stacked) {
                m_stacked = stacked;
                place();
            }
        }

    private:
        void place()
        {
            m_grid->removeWidget(m_caption);
            m_grid->removeItem(m_body);
            m_grid->removeItem(m_actions);
            m_grid->setColumnStretch(0, 0);
            m_grid->setColumnStretch(1, 0);
            // The caption lines up with the field's text, not the middle of the row, when there is more under it
            m_caption->setMinimumHeight(32);
            if (m_stacked) {
                m_caption->setMinimumWidth(0);
                m_caption->setMaximumWidth(QWIDGETSIZE_MAX);
                m_caption->setMinimumHeight(0);
                m_grid->setContentsMargins(14, 8, 8, 8);
                m_grid->addWidget(m_caption, 0, 0, 1, 2);
                m_grid->addLayout(m_body, 1, 0);
                m_grid->addLayout(m_actions, 1, 1, Qt::AlignTop);
                m_grid->setColumnStretch(0, 1);
            } else {
                m_caption->setFixedWidth(CaptionWidth);
                m_grid->setContentsMargins(14, 8, 8, 8);
                m_grid->addWidget(m_caption, 0, 0, Qt::AlignTop);
                m_grid->addLayout(m_body, 0, 1);
                m_grid->addLayout(m_actions, 0, 2, Qt::AlignTop);
                m_grid->setColumnStretch(1, 1);
            }
        }

        QWidget* m_caption;
        QVBoxLayout* m_body;
        QHBoxLayout* m_actions;
        QGridLayout* m_grid;
        bool m_stacked = false;
    };

    // Dashed button that opens the "Add field" menu
    class AddFieldButton : public QAbstractButton
    {
    public:
        explicit AddFieldButton(QWidget* parent)
            : QAbstractButton(parent)
        {
            setText(VaultEntryEditCard::tr("Add field"));
            setAccessibleName(text());
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
            setAttribute(Qt::WA_Hover);
        }

        QSize sizeHint() const override
        {
            return {240, 46};
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
            QPainterPath path;
            path.addRoundedRect(box, 14, 14);
            if (isDown() || underMouse()) {
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
            }
            QPen pen(hasFocus() ? p.text : p.strokeStrong, hasFocus() ? 2 : 1);
            if (!hasFocus()) {
                pen.setStyle(Qt::DashLine);
            }
            painter.setPen(pen);
            painter.drawPath(path);

            QFont iconFont(Typography::FontFamily::FluentIcons);
            iconFont.setPixelSize(Typography::IconSize::Compact);
            painter.setFont(iconFont);
            painter.setPen(p.accentText);
            painter.drawText(QRectF(box.left() + 14, box.top(), 18, box.height()), Qt::AlignCenter, Typography::Icons::Add);
            painter.setFont(Vault::font(Typography::FontRole::BodyStrong));
            const QRectF label = box.adjusted(42, 0, -14, 0);
            painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, text());
            const int used = painter.fontMetrics().horizontalAdvance(text()) + 16;
            painter.setFont(Vault::font(Typography::FontRole::Caption));
            painter.setPen(p.textSecondary);
            const auto hint = VaultEntryEditCard::tr("One-time code, URL, attribute, file");
            const auto elided = painter.fontMetrics().elidedText(hint, Qt::ElideRight, int(label.width()) - used);
            painter.drawText(label, Qt::AlignRight | Qt::AlignVCenter, elided);
        }
    };

    // Header of the collapsed "More options" group: chevron, title and what it holds
    class DisclosureButton : public QAbstractButton
    {
    public:
        DisclosureButton(const QString& text, const QString& summary, QWidget* parent)
            : QAbstractButton(parent)
            , m_summary(summary)
        {
            setText(text);
            setCheckable(true);
            setAccessibleName(text);
            setAccessibleDescription(summary);
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
            setAttribute(Qt::WA_Hover);
        }

        QSize sizeHint() const override
        {
            return {240, 46};
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
            QPainterPath path;
            path.addRoundedRect(box, VaultMetrics::PanelRadius, VaultMetrics::PanelRadius);
            painter.fillPath(path, p.inset);
            if (isDown() || underMouse()) {
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
            }
            painter.setPen(QPen(hasFocus() ? p.text : p.stroke, hasFocus() ? 2 : 1));
            painter.drawPath(path);

            QFont iconFont(Typography::FontFamily::FluentIcons);
            iconFont.setPixelSize(Typography::IconSize::Compact);
            painter.setFont(iconFont);
            painter.setPen(p.text);
            painter.drawText(QRectF(box.left() + 12, box.top(), 18, box.height()),
                             Qt::AlignCenter,
                             isChecked() ? Typography::Icons::ChevronDown : Typography::Icons::ChevronRight);
            painter.setFont(Vault::font(Typography::FontRole::BodyStrong));
            const QRectF label = box.adjusted(40, 0, -14, 0);
            painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, text());
            const int used = painter.fontMetrics().horizontalAdvance(text()) + 16;
            painter.setFont(Vault::font(Typography::FontRole::Caption));
            painter.setPen(p.textSecondary);
            const auto elided = painter.fontMetrics().elidedText(m_summary, Qt::ElideRight, int(label.width()) - used);
            painter.drawText(label, Qt::AlignRight | Qt::AlignVCenter, elided);
        }

    private:
        QString m_summary;
    };

    // A pill that is on or off: the entry type chips and the expiry choices
    class ChoicePill : public QAbstractButton
    {
    public:
        ChoicePill(const QString& text, QWidget* parent, bool outlined)
            : QAbstractButton(parent)
            , m_outlined(outlined)
        {
            setText(text);
            setCheckable(true);
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
            setAttribute(Qt::WA_Hover);
        }

        QSize sizeHint() const override
        {
            const QFontMetrics metrics(Vault::font(Typography::FontRole::BodyStrong));
            return {metrics.horizontalAdvance(text()) + (m_outlined ? 32 : 24), m_outlined ? 34 : 28};
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            const qreal radius = box.height() / 2;
            QPainterPath path;
            path.addRoundedRect(box, radius, radius);
            if (isChecked()) {
                painter.fillPath(path, m_outlined ? p.accentSubtle : p.layer);
            } else if (isEnabled() && (isDown() || underMouse())) {
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
            }
            if (m_outlined || hasFocus()) {
                painter.setPen(QPen(hasFocus() ? p.text : (isChecked() ? p.accent : p.stroke), hasFocus() ? 2 : 1));
                painter.drawPath(path);
            } else if (isChecked()) {
                painter.setPen(QPen(p.stroke, 1));
                painter.drawPath(path);
            }
            painter.setFont(Vault::font(isChecked() ? Typography::FontRole::BodyStrong : Typography::FontRole::Body));
            painter.setPen(!isEnabled() ? p.textDisabled
                                        : (isChecked() ? (m_outlined ? p.accentText : p.text) : p.textSecondary));
            painter.drawText(box, Qt::AlignCenter, text());
        }

    private:
        bool m_outlined;
    };

    // Lays its items out in rows, wrapping to the next row when the width runs out, so a row of pills fits
    // a narrow card
    class FlowLayout : public QLayout
    {
    public:
        explicit FlowLayout(QWidget* parent, int spacing)
            : QLayout(parent)
            , m_spacing(spacing)
        {
            setContentsMargins(0, 0, 0, 0);
        }

        ~FlowLayout() override
        {
            while (auto* item = takeAt(0)) {
                delete item;
            }
        }

        void addItem(QLayoutItem* item) override
        {
            m_items << item;
        }
        int count() const override
        {
            return m_items.size();
        }
        QLayoutItem* itemAt(int index) const override
        {
            return m_items.value(index);
        }
        QLayoutItem* takeAt(int index) override
        {
            return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
        }
        Qt::Orientations expandingDirections() const override
        {
            return {};
        }
        bool hasHeightForWidth() const override
        {
            return true;
        }
        int heightForWidth(int width) const override
        {
            return arrange(QRect(0, 0, width, 0), false);
        }
        void setGeometry(const QRect& rect) override
        {
            QLayout::setGeometry(rect);
            arrange(rect, true);
        }
        QSize sizeHint() const override
        {
            int width = 0;
            int height = 0;
            for (auto* item : m_items) {
                width += item->sizeHint().width() + m_spacing;
                height = qMax(height, item->sizeHint().height());
            }
            return {qMax(0, width - m_spacing), height};
        }
        QSize minimumSize() const override
        {
            // As narrow as its widest item: the rest wraps
            QSize size;
            for (auto* item : m_items) {
                size = size.expandedTo(item->minimumSize().expandedTo(item->sizeHint()));
            }
            return size;
        }

    private:
        int arrange(const QRect& rect, bool apply) const
        {
            int x = rect.x();
            int y = rect.y();
            int lineHeight = 0;
            for (auto* item : m_items) {
                const auto hint = item->sizeHint();
                if (x > rect.x() && x + hint.width() > rect.right() + 1) {
                    x = rect.x();
                    y += lineHeight + m_spacing;
                    lineHeight = 0;
                }
                if (apply) {
                    item->setGeometry(QRect(QPoint(x, y), hint));
                }
                x += hint.width() + m_spacing;
                lineHeight = qMax(lineHeight, hint.height());
            }
            return y + lineHeight - rect.y();
        }

        QList<QLayoutItem*> m_items;
        int m_spacing;
    };

    // One of the editor's other pages: its glyph, its name, what it holds and a chevron
    class SectionRow : public QAbstractButton
    {
    public:
        SectionRow(const QString& glyph, const QString& text, const QString& description, QWidget* parent)
            : QAbstractButton(parent)
            , m_glyph(glyph)
            , m_description(description)
        {
            setText(text);
            setAccessibleDescription(description);
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
            setAttribute(Qt::WA_Hover);
        }

        QSize sizeHint() const override
        {
            return {200, m_description.isEmpty() ? 40 : SectionRowHeight};
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);

            const QRectF row = QRectF(rect()).adjusted(3, 3, -3, -3);
            if (isDown() || underMouse()) {
                QPainterPath path;
                path.addRoundedRect(row, VaultMetrics::RowRadius, VaultMetrics::RowRadius);
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
            }
            if (hasFocus()) {
                QPainterPath path;
                path.addRoundedRect(row.adjusted(1, 1, -1, -1), VaultMetrics::RowRadius, VaultMetrics::RowRadius);
                painter.setPen(QPen(p.text, 2));
                painter.drawPath(path);
            }

            QFont iconFont(Typography::FontFamily::FluentIcons);
            iconFont.setPixelSize(Typography::IconSize::Standard);
            painter.setFont(iconFont);
            painter.setPen(p.text);
            painter.drawText(QRectF(row.left() + 10, row.top(), 20, row.height()), Qt::AlignCenter, m_glyph);
            iconFont.setPixelSize(Typography::IconSize::Compact);
            painter.setFont(iconFont);
            painter.setPen(p.textSecondary);
            painter.drawText(QRectF(row.right() - 30, row.top(), 20, row.height()),
                             Qt::AlignCenter,
                             Typography::Icons::ChevronRight);

            const QRectF texts = row.adjusted(44, 0, -40, 0);
            const auto titleFont = Vault::font(Typography::FontRole::Body);
            if (m_description.isEmpty()) {
                painter.setFont(titleFont);
                painter.setPen(p.text);
                painter.drawText(texts, Qt::AlignLeft | Qt::AlignVCenter, text());
                return;
            }
            const auto captionFont = Vault::font(Typography::FontRole::Caption);
            const int titleHeight = QFontMetrics(titleFont).height();
            const int captionHeight = QFontMetrics(captionFont).height();
            const qreal top = texts.top() + (texts.height() - titleHeight - captionHeight) / 2;
            painter.setFont(titleFont);
            painter.setPen(p.text);
            painter.drawText(QRectF(texts.left(), top, texts.width(), titleHeight),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             text());
            painter.setFont(captionFont);
            painter.setPen(p.textSecondary);
            const auto elided = QFontMetrics(captionFont).elidedText(m_description, Qt::ElideRight, int(texts.width()));
            painter.drawText(QRectF(texts.left(), top + titleHeight, texts.width(), captionHeight),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             elided);
        }

    private:
        QString m_glyph;
        QString m_description;
    };

    struct SectionInfo
    {
        const char* name;
        QString glyph;
        QString description;
    };

    // Glyph and description of each of the entry editor's pages, matched by the name the editor gives it
    QList<SectionInfo> sectionInfos()
    {
        return {
            {"Advanced", Typography::Icons::Settings, VaultEntryEditCard::tr("Colors, and every attribute and attachment")},
            {"Icon", Typography::Icons::Emoji, VaultEntryEditCard::tr("Choose an icon or use the site's")},
            {"Auto-Type", Typography::Icons::Keyboard, VaultEntryEditCard::tr("Sequences and window associations")},
            {"Browser Integration", Typography::Icons::World, VaultEntryEditCard::tr("Which sites can fill this entry")},
            {"SSH Agent", Typography::Icons::Lock, VaultEntryEditCard::tr("Keys to add to the agent")},
            {"Properties", Typography::Icons::Info, VaultEntryEditCard::tr("Dates, UUID and plugin data")},
            {"History", Typography::Icons::History, VaultEntryEditCard::tr("Earlier versions of this entry")},
        };
    }

    // Pages are wrapped in scroll areas that would otherwise paint the window color over the card
    void clearPageBackgrounds(QStackedWidget* pages)
    {
        for (int i = 0; i < pages->count(); ++i) {
            auto* page = pages->widget(i);
            page->setAutoFillBackground(false);
            if (auto* scroll = qobject_cast<QScrollArea*>(page)) {
                scroll->setFrameShape(QFrame::NoFrame);
                scroll->viewport()->setAutoFillBackground(false);
                if (scroll->widget()) {
                    scroll->widget()->setAutoFillBackground(false);
                }
            }
        }
    }

    // The form inside the Entry page
    QWidget* entryPageForm(QStackedWidget* pages)
    {
        auto* scroll = qobject_cast<QScrollArea*>(pages->widget(0));
        return scroll ? scroll->widget() : nullptr;
    }

    // Move a widget of the form to a new parent, keeping whether the editor hid it
    void adopt(QWidget* widget, QWidget* parent)
    {
        const bool hidden = widget->testAttribute(Qt::WA_WState_Hidden)
                            && widget->testAttribute(Qt::WA_WState_ExplicitShowHide);
        widget->setParent(parent);
        widget->setVisible(!hidden);
    }

    // Caption of a form label without its trailing colon
    void makeCaption(QLabel* label)
    {
        auto text = VaultButton::withoutMnemonic(label->text().trimmed());
        while (text.endsWith(QLatin1Char(':')) || text.endsWith(QChar(0xff1a))) {
            text.chop(1);
        }
        label->setText(text);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }

    // Attributes with rows of their own (the one-time code, extra URLs) or that only the app reads (passkeys)
    bool isListedAttribute(const QString& key)
    {
        static const QStringList totpKeys = {Totp::ATTRIBUTE_OTP,
                                             Totp::ATTRIBUTE_SEED,
                                             Totp::ATTRIBUTE_SETTINGS,
                                             Totp::KP2_TOTP_SECRET,
                                             Totp::KP2_TOTP_ALGORITHM,
                                             Totp::KP2_TOTP_LENGTH,
                                             Totp::KP2_TOTP_PERIOD};
        return !EntryAttributes::isDefaultAttribute(key) && !totpKeys.contains(key)
               && !key.startsWith(EntryAttributes::AdditionalUrlAttribute)
               && !EntryAttributes::isPasskeyAttribute(key);
    }

    // Extra URL attributes in order: KP2A_URL, KP2A_URL_1, KP2A_URL_2...
    QStringList additionalUrlKeys(const EntryAttributes* attributes)
    {
        QStringList keys;
        for (const auto& key : attributes->customKeys()) {
            if (key == EntryAttributes::AdditionalUrlAttribute
                || key.startsWith(EntryAttributes::AdditionalUrlAttribute + QLatin1Char('_'))) {
                keys << key;
            }
        }
        std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
            const auto number = [](const QString& key) { return key.section(QLatin1Char('_'), 2).toInt(); };
            return number(a) < number(b);
        });
        return keys;
    }

    QString formatSize(qint64 bytes)
    {
        return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
    }

    struct EntryType
    {
        QString id;
        QString label;
        QStringList fields; // standard fields shown: username, password, url
        QList<QPair<QString, bool>> attributes; // seeded attributes and whether they're hidden
    };

    QList<EntryType> entryTypes()
    {
        const auto tr = [](const char* text) { return VaultEntryEditCard::tr(text); };
        return {
            {"login", tr("Login"), {"username", "password", "url"}, {}},
            {"note", tr("Secure note"), {}, {}},
            {"card",
             tr("Card"),
             {},
             {{tr("Cardholder"), false}, {tr("Number"), false}, {tr("Expires (MM/YY)"), false}, {tr("Security code"), true}}},
            {"wifi", tr("Wi-Fi"), {"password"}, {{tr("Network name"), false}, {tr("Security"), false}}},
            {"ssh", tr("SSH key"), {"username", "password", "url"}, {}},
            {"api", tr("API key"), {"url"}, {{tr("Key"), true}, {tr("Secret"), true}}},
        };
    }
} // namespace

VaultEntryEditCard::VaultEntryEditCard(EditWidget* editor, QWidget* parent)
    : fluent::layout::Card(parent)
    , m_editor(editor)
    , m_categories(editor->categoryList())
    , m_pages(editor->pageStack())
    , m_headline(editor->headlineLabel())
    , m_totpTimer(new QTimer(this))
{
    setAppearance(fluent::layout::Card::Layer);
    setBorderVisible(true);
    setMinimumWidth(360);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    editor->setParent(this);
    outer->addWidget(editor);
    if (auto* layout = editor->layout()) {
        layout->setContentsMargins(20, 18, 20, 14);
        layout->setSpacing(14);
    }

    // The pages are reached from the rows under "More options" instead of a category list
    m_categories->hide();

    buildHeader();

    // Back row above the other pages, which open in place of the Entry page
    auto* pageHost = new QWidget(editor);
    auto* pageHostLayout = new QVBoxLayout(pageHost);
    pageHostLayout->setContentsMargins(0, 0, 0, 0);
    pageHostLayout->setSpacing(10);
    m_backRow = new QWidget(pageHost);
    auto* backLayout = new QHBoxLayout(m_backRow);
    backLayout->setContentsMargins(0, 0, 0, 0);
    backLayout->setSpacing(6);
    auto* backButton = Vault::iconButton(Typography::Icons::Back, QString(), m_backRow);
    backButton->setObjectName("vaultEditBackButton");
    backButton->setFluentSize(fluent::basicinput::Button::Small);
    if (m_categories->listWidget()->count() > 0) {
        const auto back = tr("Back to %1").arg(m_categories->listWidget()->item(0)->text());
        backButton->setToolTip(back);
        backButton->setAccessibleName(back);
    }
    connect(backButton, &QAbstractButton::clicked, this, [this] { m_categories->setCurrentCategory(0); });
    backLayout->addWidget(backButton);
    m_pageTitle = new QLabel(m_backRow);
    m_pageTitle->setTextFormat(Qt::PlainText);
    backLayout->addWidget(m_pageTitle, 1);
    pageHostLayout->addWidget(m_backRow);
    editor->layout()->replaceWidget(m_pages, pageHost);
    pageHostLayout->addWidget(m_pages, 1);
    connect(m_pages, &QStackedWidget::currentChanged, this, &VaultEntryEditCard::pageChanged);

    buildEntryPage();

    if (auto* icons = m_pages->findChild<EditWidgetIcons*>()) {
        connect(icons, &EditWidgetIcons::widgetUpdated, this, &VaultEntryEditCard::updateIdentity);
    }

    m_totpTimer->setInterval(1000);
    connect(m_totpTimer, &QTimer::timeout, this, &VaultEntryEditCard::updateTotpCode);

    clearPageBackgrounds(m_pages);
    // The editor fills itself with the window colour (DialogyWidget); the card's layer shows through instead
    m_editor->setAutoFillBackground(false);
    pageChanged(m_pages->currentIndex());
    applyColors();
}

// The entry's icon and title at the top, with where it lives beneath, staying in view on every page
void VaultEntryEditCard::buildHeader()
{
    auto* header = new QWidget(m_editor);
    header->setObjectName("vaultEditCardHeader");
    m_header = header;
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(12);

    auto* tile = new IconTile(header);
    tile->setObjectName("vaultEditIconTile");
    tile->setToolTip(tr("Change icon"));
    tile->setAccessibleName(tr("Change icon"));
    connect(tile, &QAbstractButton::clicked, this, [this] {
        auto* icons = m_pages->findChild<EditWidgetIcons*>();
        for (int i = 0; icons && i < m_pages->count(); ++i) {
            if (m_pages->widget(i)->isAncestorOf(icons)) {
                m_categories->setCurrentCategory(i);
            }
        }
    });
    m_iconTile = tile;
    headerLayout->addWidget(tile, 0, Qt::AlignTop);

    auto* titles = new QVBoxLayout();
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(2);
    headerLayout->addLayout(titles, 1);

    // The headline stays with the editor, which keeps setting it, but the title field and the subtitle say it
    auto* placeholder = new QWidget();
    placeholder->hide();
    m_headline->parentWidget()->layout()->replaceWidget(m_headline, placeholder);
    // The editor shows the headline whenever it sets it, so it waits in a hidden holder
    auto* headlineHolder = new QWidget(header);
    headlineHolder->hide();
    m_headline->setParent(headlineHolder);

    auto* form = entryPageForm(m_pages);
    m_titleEdit = form ? form->findChild<QLineEdit*>("titleEdit") : nullptr;
    if (m_titleEdit) {
        adopt(m_titleEdit, header);
        m_titleEdit->setPlaceholderText(tr("Title"));
        // As a font role, which the field applies again whenever the theme changes
        if (auto* fluentEdit = qobject_cast<fluent::textfields::LineEdit*>(m_titleEdit.data())) {
            fluentEdit->setFontRole(Typography::FontRole::Subtitle);
        }
        titles->addWidget(m_titleEdit);
        if (auto* label = form->findChild<QLabel*>("titleLabel")) {
            label->hide();
        }
    }
    m_subtitle = new QLabel(header);
    m_subtitle->setObjectName("vaultEditSubtitle");
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setContentsMargins(2, 0, 0, 0);
    titles->addWidget(m_subtitle);

    // Cancel and Save sit in the command bar instead. The bar keeps the editor's buttons shown, so Enter
    // and Escape still reach them. Save returns to the entry's details, so there is no need to apply and stay.
    m_buttonBar = new VaultButtonBar(m_editor->buttonBox(), header);
    for (auto button : {QDialogButtonBox::Ok, QDialogButtonBox::Cancel, QDialogButtonBox::Close, QDialogButtonBox::Apply}) {
        m_buttonBar->setButtonHidden(button, true);
    }
    headerLayout->addWidget(m_buttonBar, 0, Qt::AlignTop);
    qobject_cast<QBoxLayout*>(m_editor->layout())->insertWidget(0, header);
}

// The detail card's groups, each field with its caption beside it, then "Add field" and the other pages
void VaultEntryEditCard::buildEntryPage()
{
    auto* form = entryPageForm(m_pages);
    if (!form) {
        return;
    }
    auto find = [form](const char* name) { return form->findChild<QWidget*>(QString::fromLatin1(name)); };
    auto label = [form](const char* name) { return form->findChild<QLabel*>(QString::fromLatin1(name)); };

    auto* body = new QWidget(form);
    body->setObjectName("vaultEditEntryPage");
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(GroupGap);

    auto group = [body](const char* name) {
        auto* box = new InsetGroup(body, true);
        box->setObjectName(QString::fromLatin1(name));
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setSpacing(0);
        return box;
    };
    auto captioned = [this](QLabel* caption) {
        makeCaption(caption);
        m_captions << caption;
        return caption;
    };
    // A form control, moved into a row
    auto row = [&](QWidget* parent, QLabel* caption, QWidget* control, const QWidgetList& actions = {}) {
        auto* editRow = new EditRow(captioned(caption), parent);
        adopt(control, editRow);
        editRow->addBody(control);
        for (auto* action : actions) {
            adopt(action, editRow);
            editRow->addAction(action);
        }
        parent->layout()->addWidget(editRow);
        return editRow;
    };
    auto heading = [this, body](const QString& text, const char* name) {
        auto* headingLabel = new QLabel(text, body);
        headingLabel->setObjectName(QString::fromLatin1(name));
        headingLabel->setContentsMargins(2, HeadingGap, 0, 0);
        m_headings << headingLabel;
        return headingLabel;
    };

    // A new entry starts from a type, which picks the fields it starts with
    m_typeRow = new QWidget(body);
    m_typeRow->setObjectName("vaultEditTypes");
    auto* typeFlow = new FlowLayout(m_typeRow, 8);
    auto* typeChoices = new QButtonGroup(m_typeRow);
    typeChoices->setExclusive(true);
    const auto types = entryTypes();
    for (int i = 0; i < types.size(); ++i) {
        auto* chip = new ChoicePill(types[i].label, m_typeRow, true);
        chip->setObjectName("vaultEditType_" + types[i].id);
        chip->setProperty(KeyProperty, types[i].id);
        typeChoices->addButton(chip, i);
        typeFlow->addWidget(chip);
        m_typeChips << chip;
        connect(chip, &QAbstractButton::clicked, this, [this, id = types[i].id] { applyType(id); });
    }
    m_typeRow->hide();
    layout->addWidget(m_typeRow);

    // Sign-in: username, password, one-time code and the URLs
    m_signIn = group("vaultEditSignIn");
    m_signInLayout = qobject_cast<QVBoxLayout*>(m_signIn->layout());
    m_userRow = row(m_signIn, label("usernameLabel"), find("usernameComboBox"));
    m_passwordRow = row(m_signIn, label("passwordLabel"), find("passwordEdit"));
    m_urlRow = row(m_signIn, label("urlLabel"), find("urlEdit"), {find("fetchFaviconButton")});
    layout->addWidget(m_signIn);

    // Attributes, one row each
    m_attributesHeading = heading(tr("Additional attributes"), "vaultEditAttributesHeading");
    layout->addWidget(m_attributesHeading);
    m_attributesGroup = group("vaultEditAttributes");
    m_attributesLayout = qobject_cast<QVBoxLayout*>(m_attributesGroup->layout());
    layout->addWidget(m_attributesGroup);

    // Details: tags, expiry and attachments
    layout->addWidget(heading(tr("Details"), "vaultEditDetailsHeading"));
    m_detailsGroup = group("vaultEditDetails");
    m_detailsLayout = qobject_cast<QVBoxLayout*>(m_detailsGroup->layout());
    row(m_detailsGroup, label("tagsLabel"), find("tagsList"));

    // Expiry: presets first, a date only when asked for. The editor's check box and date still hold the value.
    auto* expiryLabel = label("expireLabel");
    auto* expiryRow = new EditRow(captioned(expiryLabel), m_detailsGroup);
    expiryRow->setObjectName("vaultEditExpiry");
    auto* choices = new QWidget(expiryRow);
    choices->setObjectName("vaultEditExpiryChoices");
    auto* choicesLayout = new FlowLayout(choices, 6);
    m_expiryChoices = new QButtonGroup(choices);
    m_expiryChoices->setExclusive(true);
    const QStringList expiryTexts = {tr("Never"), tr("In 3 months"), tr("In 1 year"), tr("Pick a date…")};
    for (int i = 0; i < expiryTexts.size(); ++i) {
        auto* pill = new ChoicePill(expiryTexts[i], choices, true);
        pill->setObjectName(QStringLiteral("vaultEditExpiry%1").arg(i));
        m_expiryChoices->addButton(pill, i);
        choicesLayout->addWidget(pill);
    }
    connect(m_expiryChoices, &QButtonGroup::idClicked, this, &VaultEntryEditCard::setExpiry);
    expiryRow->addBody(choices);
    auto* datePicker = find("expireDatePicker");
    adopt(datePicker, expiryRow);
    datePicker->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    expiryRow->addBody(datePicker);
    m_expiryNote = new QLabel(expiryRow);
    m_expiryNote->setObjectName("vaultEditExpiryNote");
    m_notes << m_expiryNote;
    expiryRow->addBody(m_expiryNote);
    if (auto* date = qobject_cast<QDateTimeEdit*>(datePicker)) {
        connect(date, &QDateTimeEdit::dateTimeChanged, this, &VaultEntryEditCard::updateExpiryNote);
    }
    // The check box and the presets menu stay with the rest of the form, out of sight
    if (auto* check = qobject_cast<QAbstractButton*>(find("expireCheck"))) {
        connect(check, &QAbstractButton::toggled, this, &VaultEntryEditCard::updateExpiryNote);
    }
    m_detailsLayout->addWidget(expiryRow);
    layout->addWidget(m_detailsGroup);

    // Notes, with the button that reveals them beside their heading
    auto* notesHeader = new QWidget(body);
    auto* notesHeaderLayout = new QHBoxLayout(notesHeader);
    notesHeaderLayout->setContentsMargins(0, 0, 0, 0);
    auto* notesLabel = label("notesLabel");
    makeCaption(notesLabel);
    adopt(notesLabel, notesHeader);
    notesLabel->setContentsMargins(2, HeadingGap, 0, 0);
    m_headings << notesLabel;
    notesHeaderLayout->addWidget(notesLabel);
    notesHeaderLayout->addStretch();
    auto* revealNotes = find("revealNotesButton");
    adopt(revealNotes, notesHeader);
    notesHeaderLayout->addWidget(revealNotes, 0, Qt::AlignBottom);
    layout->addWidget(notesHeader);
    auto* notes = find("notesEdit");
    adopt(notes, body);
    notes->setMinimumHeight(NotesMinHeight);
    notes->setMaximumHeight(NotesMaxHeight);
    notes->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(notes);

    // One way to add everything optional
    m_addButton = new AddFieldButton(body);
    m_addButton->setObjectName("vaultEditAddField");
    connect(m_addButton, &QAbstractButton::clicked, this, &VaultEntryEditCard::populateAddMenu);
    layout->addWidget(m_addButton);

    // The editor's other pages, under a collapsed group
    m_moreToggle = new DisclosureButton(tr("More options"), tr("Auto-Type, browser, SSH agent, history"), body);
    m_moreToggle->setObjectName("vaultEditMoreOptions");
    layout->addWidget(m_moreToggle);
    m_sections = new InsetGroup(body, true);
    m_sections->setObjectName("vaultEditSections");
    m_sectionsLayout = new QVBoxLayout(m_sections);
    m_sectionsLayout->setContentsMargins(0, 0, 0, 0);
    m_sectionsLayout->setSpacing(0);
    m_sections->hide();
    layout->addWidget(m_sections);
    connect(m_moreToggle, &QAbstractButton::toggled, m_sections, &QWidget::setVisible);
    layout->addStretch();

    // Keep what's left of the form, its labels and layouts, out of sight
    auto* rest = new QWidget(form);
    rest->setObjectName("vaultEditFormRest");
    rest->setLayout(form->layout());
    rest->hide();
    auto* formLayout = new QVBoxLayout(form);
    formLayout->setContentsMargins(0, 0, 0, 0);
    formLayout->addWidget(body);

    if (auto* urlEdit = qobject_cast<QLineEdit*>(find("urlEdit"))) {
        connect(urlEdit, &QLineEdit::textEdited, this, &VaultEntryEditCard::suggestTitleFromUrl);
    }
}

EditWidget* VaultEntryEditCard::editor() const
{
    return m_editor;
}

EditEntryWidget* VaultEntryEditCard::entryEditor() const
{
    return qobject_cast<EditEntryWidget*>(m_editor);
}

bool VaultEntryEditCard::readOnly() const
{
    return m_editor->readOnly();
}

void VaultEntryEditCard::prepare(bool create)
{
    finish();
    m_create = create;
    m_suggestedTitle.clear();
    m_seededKeys.clear();
    m_hiddenFields.clear();
    m_type.clear();

    if (auto* editor = entryEditor()) {
        m_attributes = editor->editedAttributes();
        m_attachments = editor->editedAttachments();
    }
    if (m_attributes) {
        for (auto signal : {&EntryAttributes::added, &EntryAttributes::removed}) {
            m_following << connect(m_attributes, signal, this, &VaultEntryEditCard::scheduleRebuild);
        }
        m_following << connect(m_attributes, &EntryAttributes::renamed, this, &VaultEntryEditCard::scheduleRebuild);
        m_following << connect(m_attributes, &EntryAttributes::reset, this, &VaultEntryEditCard::scheduleRebuild);
        // The Advanced page edits values too; the rows follow unless they are being typed in
        m_following << connect(m_attributes, &EntryAttributes::customKeyModified, this, [this](const QString& key) {
            for (auto* field : findChildren<QLineEdit*>()) {
                if (field->property(KeyProperty).toString() == key && field->property(PartProperty) == "value"
                    && !field->hasFocus()) {
                    field->setText(m_attributes->value(key));
                }
            }
            if (key == totpKey()) {
                updateTotpCode();
            }
        });
    }
    if (m_attachments) {
        for (auto signal : {&EntryAttachments::added, &EntryAttachments::removed}) {
            m_following << connect(m_attachments, signal, this, &VaultEntryEditCard::scheduleRebuild);
        }
        m_following << connect(m_attachments, &EntryAttachments::reset, this, &VaultEntryEditCard::scheduleRebuild);
    }

    const bool editable = !readOnly();
    m_typeRow->setVisible(create && editable);
    m_addButton->setVisible(editable);
    for (auto* pill : m_expiryChoices->buttons()) {
        pill->setEnabled(editable);
    }
    for (auto* row : {m_userRow, m_passwordRow, m_urlRow}) {
        row->show();
    }
    m_signIn->show();
    if (create && editable) {
        m_typeChips.first()->setChecked(true);
        m_type = QStringLiteral("login");
    }

    // Expiry: a date the entry has shows as a picked date
    auto* check = m_pages->widget(0)->findChild<QAbstractButton*>("expireCheck");
    m_expiryChoices->button(check && check->isChecked() ? ExpiresOnDate : ExpiresNever)->setChecked(true);
    if (auto* picker = m_pages->widget(0)->findChild<QWidget*>("expireDatePicker")) {
        picker->setVisible(check && check->isChecked());
    }
    updateExpiryNote();

    rebuildSections();
    rebuildDynamicRows();
    updateIdentity();
    pageChanged(m_pages->currentIndex());
    if (m_titleEdit && editable) {
        QTimer::singleShot(0, m_titleEdit, [title = m_titleEdit] { title->setFocus(); });
    }
}

void VaultEntryEditCard::finish()
{
    for (const auto& connection : m_following) {
        disconnect(connection);
    }
    m_following.clear();
    m_totpTimer->stop();
}

void VaultEntryEditCard::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    Vault::paintCardSurface(this, VaultMetrics::CardRadius, isBorderVisible());
}

void VaultEntryEditCard::resizeEvent(QResizeEvent* event)
{
    fluent::layout::Card::resizeEvent(event);
    setStacked(width() < StackedBelow);
}

void VaultEntryEditCard::setStacked(bool stacked)
{
    m_stacked = stacked;
    for (auto* widget : findChildren<QWidget*>()) {
        if (auto* row = dynamic_cast<EditRow*>(widget)) {
            row->setStacked(stacked);
        }
    }
}

void VaultEntryEditCard::onThemeUpdated()
{
    fluent::layout::Card::onThemeUpdated();
    applyColors();
    updateIdentity();
}

void VaultEntryEditCard::rebuildSections()
{
    while (auto* item = m_sectionsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto infos = sectionInfos();
    auto* list = m_categories->listWidget();
    for (int i = 1; i < list->count(); ++i) {
        if (m_categories->isCategoryHidden(i)) {
            continue;
        }
        const auto text = list->item(i)->text();
        QString glyph = Typography::Icons::More;
        QString description;
        for (const auto& info : infos) {
            if (text == QCoreApplication::translate("EditEntryWidget", info.name)) {
                glyph = info.glyph;
                description = info.description;
            }
        }
        auto* row = new SectionRow(glyph, text, description, m_sections);
        connect(row, &QAbstractButton::clicked, this, [this, i] { m_categories->setCurrentCategory(i); });
        m_sectionsLayout->addWidget(row);
    }
    m_moreToggle->setVisible(m_sectionsLayout->count() > 0);
}

void VaultEntryEditCard::pageChanged(int index)
{
    m_backRow->setVisible(index > 0);
    auto* item = m_categories->listWidget()->item(index);
    m_pageTitle->setText(item ? item->text() : QString());
    // Coming back from another page, the rows catch up with what it changed
    if (index == 0 && isFollowing()) {
        scheduleRebuild();
    }
}

void VaultEntryEditCard::scheduleRebuild()
{
    if (m_rebuildPending) {
        return;
    }
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this] {
        m_rebuildPending = false;
        // Editing may have ended meanwhile, and the editor cleared what the rows would show
        if (isFollowing()) {
            rebuildDynamicRows();
        }
    });
}

void VaultEntryEditCard::rebuildDynamicRows()
{
    // Remember the field being typed in, to put the caret back once its row is built again
    if (auto* focused = qobject_cast<QLineEdit*>(QApplication::focusWidget()); focused && isAncestorOf(focused)) {
        const auto key = focused->property(KeyProperty).toString();
        if (!key.isEmpty()) {
            m_focusKey = key;
            m_focusPart = focused->property(PartProperty).toString();
        }
    }
    rebuildSignInRows();
    rebuildAttributeRows();
    rebuildAttachmentRows();
    setStacked(m_stacked);
    styleRowLabels();
    // New rows take the form's style sheet as they show, which resets their colors, so color them after
    QTimer::singleShot(0, this, &VaultEntryEditCard::styleRowLabels);
    if (!m_focusKey.isEmpty()) {
        for (auto* field : findChildren<QLineEdit*>()) {
            if (field->property(KeyProperty).toString() == m_focusKey && field->property(PartProperty) == m_focusPart) {
                field->setFocus();
            }
        }
        m_focusKey.clear();
        m_focusPart.clear();
    }
}

QString VaultEntryEditCard::totpKey() const
{
    if (!m_attributes) {
        return {};
    }
    for (const auto& key : {Totp::ATTRIBUTE_OTP, Totp::ATTRIBUTE_SEED, Totp::KP2_TOTP_SECRET}) {
        if (m_attributes->contains(key)) {
            return key;
        }
    }
    return {};
}

// The one-time code after the password, and the extra URLs after the URL
void VaultEntryEditCard::rebuildSignInRows()
{
    for (auto* widget : m_signInExtras) {
        m_signInLayout->removeWidget(widget);
        widget->hide();
        widget->deleteLater();
    }
    m_signInExtras.clear();
    m_totpEdit = nullptr;
    m_totpCode = nullptr;
    m_totpTimer->stop();
    if (!m_attributes) {
        return;
    }
    const bool editable = !readOnly();
    auto removeButton = [](QWidget* parent, const QString& toolTip) {
        auto* button = Vault::iconButton(Typography::Icons::Cancel, toolTip, parent);
        button->setFluentSize(fluent::basicinput::Button::Small);
        return button;
    };

    const auto key = totpKey();
    if (!key.isEmpty()) {
        auto* caption = new QLabel(tr("One-time code"));
        m_captions << caption;
        auto* row = new EditRow(caption, m_signIn);
        row->setObjectName("vaultEditTotp");
        m_totpEdit = new VaultLineEdit(row);
        m_totpEdit->setObjectName("vaultEditTotpEdit");
        m_totpEdit->setProperty(KeyProperty, key);
        m_totpEdit->setProperty(PartProperty, "value");
        m_totpEdit->setPlaceholderText(tr("Secret or otpauth:// link"));
        m_totpEdit->setFont(Font::fixedFont());
        m_totpEdit->setText(m_attributes->value(key));
        m_totpEdit->setCursorPosition(0);
        m_totpEdit->setReadOnly(!editable);
        m_totpEdit->setAccessibleName(tr("One-time code secret"));
        connect(m_totpEdit, &QLineEdit::textEdited, this, [this, key](const QString& text) {
            m_attributes->set(key, text.trimmed(), true);
            entryEditor()->markModified();
            updateTotpCode();
        });
        connect(m_totpEdit, &QLineEdit::editingFinished, this, &VaultEntryEditCard::normalizeTotp);
        row->addBody(m_totpEdit);
        m_totpCode = new QLabel(row);
        m_totpCode->setObjectName("vaultEditTotpCode");
        m_notes << m_totpCode;
        row->addBody(m_totpCode);
        if (editable) {
            auto* remove = removeButton(row, tr("Remove one-time code"));
            remove->setObjectName("vaultEditRemoveTotp");
            connect(remove, &QAbstractButton::clicked, this, [this] {
                for (const auto& totp : {Totp::ATTRIBUTE_OTP,
                                         Totp::ATTRIBUTE_SEED,
                                         Totp::ATTRIBUTE_SETTINGS,
                                         Totp::KP2_TOTP_SECRET,
                                         Totp::KP2_TOTP_ALGORITHM,
                                         Totp::KP2_TOTP_LENGTH,
                                         Totp::KP2_TOTP_PERIOD}) {
                    m_attributes->remove(totp);
                }
                entryEditor()->markModified();
            });
            row->addAction(remove);
        }
        m_signInLayout->insertWidget(m_signInLayout->indexOf(m_passwordRow) + 1, row);
        m_signInExtras << row;
        updateTotpCode();
        m_totpTimer->start();
    }

    const auto urlKeys = additionalUrlKeys(m_attributes);
    int at = m_signInLayout->indexOf(m_urlRow) + 1;
    for (int i = 0; i < urlKeys.size(); ++i) {
        const auto& urlKey = urlKeys[i];
        auto* caption = new QLabel(tr("URL %1").arg(i + 2));
        m_captions << caption;
        auto* row = new EditRow(caption, m_signIn);
        row->setObjectName("vaultEditUrl_" + urlKey);
        auto* edit = new VaultLineEdit(row);
        edit->setProperty(KeyProperty, urlKey);
        edit->setProperty(PartProperty, "value");
        edit->setPlaceholderText(QStringLiteral("https://"));
        edit->setAccessibleName(caption->text());
        // The old editor marks a URL it just added with a placeholder
        const auto value = m_attributes->value(urlKey);
        edit->setText(value == EditEntryWidget::tr("<empty URL>") ? QString() : value);
        edit->setCursorPosition(0);
        edit->setReadOnly(!editable);
        connect(edit, &QLineEdit::textEdited, this, [this, urlKey](const QString& text) {
            m_attributes->set(urlKey, text.trimmed());
            entryEditor()->markModified();
        });
        row->addBody(edit);
        if (editable) {
            auto* remove = removeButton(row, tr("Remove URL"));
            connect(remove, &QAbstractButton::clicked, this, [this, urlKey] {
                m_attributes->remove(urlKey);
                entryEditor()->markModified();
            });
            row->addAction(remove);
        }
        m_signInLayout->insertWidget(at++, row);
        m_signInExtras << row;
    }

    // A type that starts without some sign-in fields hides them, and the group when it has none
    m_userRow->setVisible(!m_hiddenFields.contains("username"));
    m_passwordRow->setVisible(!m_hiddenFields.contains("password"));
    m_urlRow->setVisible(!m_hiddenFields.contains("url"));
    bool anyShown = !m_signInExtras.isEmpty();
    for (auto* row : {m_userRow, m_passwordRow, m_urlRow}) {
        anyShown = anyShown || !row->isHidden();
    }
    m_signIn->setVisible(anyShown);
    m_signIn->update();
}

void VaultEntryEditCard::rebuildAttributeRows()
{
    while (auto* item = m_attributesLayout->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    QStringList keys;
    if (m_attributes) {
        for (const auto& key : m_attributes->customKeys()) {
            if (isListedAttribute(key)) {
                keys << key;
            }
        }
    }
    const bool editable = !readOnly();
    // The row's fields and buttons carry the attribute's name, which a rename changes at once
    const auto keyOf = [](QObject* object) { return object->property(KeyProperty).toString(); };
    for (const auto& key : keys) {
        const bool isProtected = m_attributes->isProtected(key);

        // The name is the caption, and can be edited there
        auto* name = new VaultLineEdit();
        name->setObjectName("vaultEditAttributeName");
        name->setProperty(KeyProperty, key);
        name->setProperty(PartProperty, "name");
        name->setText(key);
        // Long names show their start, as a caption would
        name->setCursorPosition(0);
        name->setAccessibleName(tr("Attribute name"));
        name->setReadOnly(!editable);
        auto* row = new EditRow(name, m_attributesGroup);
        row->setObjectName("vaultEditAttribute");
        connect(name, &QLineEdit::editingFinished, this, [this, name, row, keyOf] {
            const auto oldKey = keyOf(name);
            const auto newKey = name->text().trimmed();
            if (newKey == oldKey || !m_attributes || !m_attributes->contains(oldKey)) {
                return;
            }
            if (newKey.isEmpty() || m_attributes->contains(newKey) || !isListedAttribute(newKey)) {
                name->setText(oldKey);
                return;
            }
            for (auto* object : row->findChildren<QObject*>()) {
                if (keyOf(object) == oldKey) {
                    object->setProperty(KeyProperty, newKey);
                }
            }
            if (auto* value = row->findChild<QLineEdit*>("vaultEditAttributeValue")) {
                value->setAccessibleName(newKey);
            }
            m_attributes->rename(oldKey, newKey);
            entryEditor()->markModified();
        });

        auto* value = new VaultLineEdit(row);
        value->setObjectName("vaultEditAttributeValue");
        value->setProperty(KeyProperty, key);
        value->setProperty(PartProperty, "value");
        value->setText(m_attributes->value(key));
        value->setCursorPosition(0);
        value->setEchoMode(isProtected ? QLineEdit::Password : QLineEdit::Normal);
        value->setAccessibleName(key);
        value->setPlaceholderText(tr("Value"));
        value->setReadOnly(!editable);
        connect(value, &QLineEdit::textEdited, this, [this, value, keyOf](const QString& text) {
            const auto current = keyOf(value);
            m_attributes->set(current, text, m_attributes->isProtected(current));
            entryEditor()->markModified();
        });
        row->addBody(value);

        auto* protect = Vault::iconButton(isProtected ? Typography::Icons::Lock : Typography::Icons::Unlock,
                                          isProtected ? tr("Hidden: stored protected. Click to show it")
                                                      : tr("Hide this value and store it protected"),
                                          row);
        protect->setObjectName("vaultEditAttributeProtect");
        protect->setProperty(KeyProperty, key);
        protect->setFluentSize(fluent::basicinput::Button::Small);
        protect->setEnabled(editable);
        connect(protect, &QAbstractButton::clicked, this, [this, protect, keyOf] {
            const auto current = keyOf(protect);
            m_attributes->set(current, m_attributes->value(current), !m_attributes->isProtected(current));
            entryEditor()->markModified();
            scheduleRebuild();
        });
        row->addAction(protect);
        if (editable) {
            auto* remove = Vault::iconButton(Typography::Icons::Delete, tr("Remove attribute"), row);
            remove->setObjectName("vaultEditAttributeRemove");
            remove->setProperty(KeyProperty, key);
            remove->setFluentSize(fluent::basicinput::Button::Small);
            connect(remove, &QAbstractButton::clicked, this, [this, remove, keyOf] {
                m_attributes->remove(keyOf(remove));
                entryEditor()->markModified();
            });
            row->addAction(remove);
        }
        m_attributesLayout->addWidget(row);
    }
    m_attributesHeading->setVisible(!keys.isEmpty());
    m_attributesGroup->setVisible(!keys.isEmpty());
    m_attributesGroup->update();
}

void VaultEntryEditCard::rebuildAttachmentRows()
{
    for (auto* widget : m_attachmentRows) {
        m_detailsLayout->removeWidget(widget);
        widget->hide();
        widget->deleteLater();
    }
    m_attachmentRows.clear();
    if (!m_attachments) {
        return;
    }
    const bool editable = !readOnly();
    for (const auto& key : m_attachments->keys()) {
        auto* caption = new QLabel(tr("Attachment"));
        m_captions << caption;
        auto* row = new EditRow(caption, m_detailsGroup);
        row->setObjectName("vaultEditAttachment");
        auto* nameLine = new QHBoxLayout();
        nameLine->setContentsMargins(0, 0, 0, 0);
        nameLine->setSpacing(8);
        auto* name = new QLabel(key, row);
        name->setTextFormat(Qt::PlainText);
        name->setMinimumHeight(32);
        auto* size = new QLabel(formatSize(m_attachments->value(key).size()), row);
        m_notes << size;
        nameLine->addWidget(name);
        nameLine->addWidget(size);
        nameLine->addStretch();
        row->addBodyLayout(nameLine);
        if (editable) {
            auto* remove = Vault::iconButton(Typography::Icons::Cancel, tr("Remove %1").arg(key), row);
            remove->setFluentSize(fluent::basicinput::Button::Small);
            connect(remove, &QAbstractButton::clicked, this, [this, key] {
                m_attachments->remove(key);
                entryEditor()->markModified();
            });
            row->addAction(remove);
        }
        m_detailsLayout->addWidget(row);
        m_attachmentRows << row;
    }
    m_detailsGroup->update();
}

void VaultEntryEditCard::updateTotpCode()
{
    if (!m_totpCode || !m_attributes) {
        return;
    }
    const auto key = totpKey();
    QSharedPointer<Totp::Settings> settings;
    if (key == Totp::ATTRIBUTE_SEED) {
        settings = Totp::parseSettings(m_attributes->value(Totp::ATTRIBUTE_SETTINGS), m_attributes->value(key));
    } else if (key == Totp::KP2_TOTP_SECRET) {
        settings = Totp::fromKeePass2Totp(m_attributes->value(key),
                                          m_attributes->value(Totp::KP2_TOTP_ALGORITHM),
                                          m_attributes->value(Totp::KP2_TOTP_LENGTH),
                                          m_attributes->value(Totp::KP2_TOTP_PERIOD));
    } else if (!key.isEmpty()) {
        const auto value = m_attributes->value(key);
        settings = value.startsWith(QLatin1String("otpauth://")) ? Totp::parseSettings(value)
                                                                  : Totp::createSettings(QString(value).remove(' '));
    }
    bool valid = false;
    const auto code = settings && !m_attributes->value(key).isEmpty() ? Totp::generateTotp(settings, &valid) : QString();
    if (!valid || code.isEmpty()) {
        m_totpCode->setText(m_attributes->value(key).isEmpty() ? tr("Paste the secret the site shows when you turn on two-step sign-in")
                                                               : tr("Not a valid secret yet"));
        return;
    }
    QString spaced = code;
    if (spaced.size() == 6 || spaced.size() == 8) {
        spaced.insert(spaced.size() / 2, QLatin1Char(' '));
    }
    const auto step = qMax<uint>(1, settings->step);
    const auto left = step - (QDateTime::currentSecsSinceEpoch() % step);
    m_totpCode->setText(tr("Code now %1 · %2 s left").arg(spaced).arg(left));
}

// A bare secret becomes an otpauth:// link, which every app reads
void VaultEntryEditCard::normalizeTotp()
{
    if (!m_totpEdit || !m_attributes || totpKey() != Totp::ATTRIBUTE_OTP) {
        return;
    }
    const auto value = m_attributes->value(Totp::ATTRIBUTE_OTP).trimmed();
    if (value.isEmpty() || value.startsWith(QLatin1String("otpauth://"))) {
        return;
    }
    auto settings = Totp::createSettings(QString(value).remove(' '));
    if (!settings || !Totp::checkValidSettings(settings).isEmpty()) {
        return;
    }
    auto* entry = entryEditor() ? entryEditor()->currentEntry() : nullptr;
    const auto title = m_titleEdit ? m_titleEdit->text() : QString();
    const auto username = m_pages->widget(0)->findChild<QComboBox*>("usernameComboBox");
    const auto link = Totp::writeSettings(
        settings, title.isEmpty() && entry ? entry->title() : title, username ? username->currentText() : QString(), true);
    if (link.isEmpty()) {
        return;
    }
    m_attributes->set(Totp::ATTRIBUTE_OTP, link, true);
    m_totpEdit->setText(link);
    m_totpEdit->setCursorPosition(0);
    updateTotpCode();
}

void VaultEntryEditCard::applyType(const QString& type)
{
    if (!m_create || type == m_type || !m_attributes) {
        return;
    }
    m_type = type;
    for (const auto& entryType : entryTypes()) {
        if (entryType.id != type) {
            continue;
        }
        // Attributes another type added and nobody filled in go
        for (const auto& key : m_seededKeys) {
            if (m_attributes->contains(key) && m_attributes->value(key).isEmpty()) {
                m_attributes->remove(key);
            }
        }
        m_seededKeys.clear();
        for (const auto& [key, hidden] : entryType.attributes) {
            if (!m_attributes->contains(key)) {
                m_attributes->set(key, QString(), hidden);
                m_seededKeys << key;
            }
        }
        m_hiddenFields.clear();
        for (const auto& field : {QStringLiteral("username"), QStringLiteral("password"), QStringLiteral("url")}) {
            if (!entryType.fields.contains(field)) {
                m_hiddenFields.insert(field);
            }
        }
        // A password that waits out of sight would still be saved, so a type without one clears it
        if (m_hiddenFields.contains("password")) {
            if (auto* password = m_pages->widget(0)->findChild<QWidget*>("passwordEdit")) {
                password->setProperty("text", QString());
            }
        }
    }
    rebuildDynamicRows();
}

void VaultEntryEditCard::populateAddMenu()
{
    auto* menu = new VaultMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto showField = [this](const QString& field, const char* focusName) {
        m_hiddenFields.remove(field);
        rebuildSignInRows();
        setStacked(m_stacked);
        if (auto* widget = m_pages->widget(0)->findChild<QWidget*>(QString::fromLatin1(focusName))) {
            focusLater(widget);
        }
    };
    if (m_hiddenFields.contains("username")) {
        menu->addAction(tr("Username"), this, [showField] { showField("username", "usernameComboBox"); });
    }
    if (m_hiddenFields.contains("password")) {
        menu->addAction(tr("Password"), this, [showField] { showField("password", "passwordEdit"); });
    }
    if (m_hiddenFields.contains("url")) {
        menu->addAction(tr("URL"), this, [showField] { showField("url", "urlEdit"); });
    }
    if (totpKey().isEmpty()) {
        menu->addAction(tr("One-time code"), this, &VaultEntryEditCard::addTotp)->setObjectName("vaultEditAddTotp");
    }
    if (!m_hiddenFields.contains("url")) {
        menu->addAction(tr("Another URL"), this, &VaultEntryEditCard::addUrl)->setObjectName("vaultEditAddUrl");
    }
    menu->addAction(tr("Text attribute"), this, [this] { addAttribute(false); })->setObjectName("vaultEditAddText");
    menu->addAction(tr("Hidden attribute"), this, [this] { addAttribute(true); })->setObjectName("vaultEditAddHidden");
    menu->addAction(tr("Attachment…"), this, [this] {
        if (auto* editor = entryEditor()) {
            editor->addAttachments();
        }
    });
    menu->setObjectName("vaultEditAddMenu");
    menu->popup(m_addButton->mapToGlobal(QPoint(0, m_addButton->height() + 4)));
}

void VaultEntryEditCard::addAttribute(bool hidden)
{
    if (!m_attributes) {
        return;
    }
    const auto base = hidden ? tr("Hidden attribute") : tr("New attribute");
    auto key = base;
    for (int i = 2; m_attributes->contains(key); ++i) {
        key = QStringLiteral("%1 %2").arg(base).arg(i);
    }
    m_attributes->set(key, QString(), hidden);
    entryEditor()->markModified();
    rebuildDynamicRows();
    for (auto* field : findChildren<QLineEdit*>()) {
        if (field->property(KeyProperty).toString() == key && field->property(PartProperty) == "name") {
            focusLater(field);
            field->selectAll();
        }
    }
}

void VaultEntryEditCard::addUrl()
{
    if (!m_attributes) {
        return;
    }
    auto key = EntryAttributes::AdditionalUrlAttribute;
    for (int i = 1; m_attributes->contains(key); ++i) {
        key = QStringLiteral("%1_%2").arg(EntryAttributes::AdditionalUrlAttribute).arg(i);
    }
    m_attributes->set(key, QString());
    entryEditor()->markModified();
    rebuildDynamicRows();
    for (auto* field : findChildren<QLineEdit*>()) {
        if (field->property(KeyProperty).toString() == key) {
            focusLater(field);
        }
    }
}

void VaultEntryEditCard::addTotp()
{
    if (!m_attributes) {
        return;
    }
    m_attributes->set(Totp::ATTRIBUTE_OTP, QString(), true);
    entryEditor()->markModified();
    rebuildDynamicRows();
    if (m_totpEdit) {
        focusLater(m_totpEdit);
    }
}

void VaultEntryEditCard::focusLater(QWidget* widget)
{
    QTimer::singleShot(0, widget, [widget] { widget->setFocus(); });
}

void VaultEntryEditCard::setExpiry(int choice)
{
    auto* page = m_pages->widget(0);
    auto* check = page->findChild<QAbstractButton*>("expireCheck");
    auto* picker = page->findChild<QDateTimeEdit*>("expireDatePicker");
    if (!check || !picker) {
        return;
    }
    switch (choice) {
    case ExpiresNever:
        check->setChecked(false);
        break;
    case ExpiresIn3Months:
        check->setChecked(true);
        picker->setDateTime(QDateTime::currentDateTime().addMonths(3));
        break;
    case ExpiresIn1Year:
        check->setChecked(true);
        picker->setDateTime(QDateTime::currentDateTime().addYears(1));
        break;
    case ExpiresOnDate:
        check->setChecked(true);
        if (picker->dateTime() <= QDateTime::currentDateTime()) {
            picker->setDateTime(QDateTime::currentDateTime().addMonths(1));
        }
        picker->setFocus();
        break;
    }
    picker->setVisible(choice == ExpiresOnDate);
    updateExpiryNote();
}

void VaultEntryEditCard::updateExpiryNote()
{
    auto* page = m_pages->widget(0);
    auto* check = page->findChild<QAbstractButton*>("expireCheck");
    auto* picker = page->findChild<QDateTimeEdit*>("expireDatePicker");
    if (!check || !picker || !m_expiryNote) {
        return;
    }
    if (!check->isChecked()) {
        m_expiryNote->setText(tr("This entry never expires"));
        return;
    }
    const auto when = picker->dateTime();
    const auto date = QLocale().toString(when.date(), QLocale::LongFormat);
    m_expiryNote->setText(when <= QDateTime::currentDateTime() ? tr("Expired %1").arg(date) : tr("Expires %1").arg(date));
}

// The icon the entry has, or the one picked on the Icon page, and where the entry lives
void VaultEntryEditCard::updateIdentity()
{
    auto* editor = entryEditor();
    auto* entry = editor ? editor->currentEntry() : nullptr;
    QPixmap pixmap;
    if (auto* icons = m_pages->findChild<EditWidgetIcons*>(); icons && entry) {
        const auto state = icons->state();
        if (!state.uuid.isNull() && entry->database()) {
            pixmap = Icons::customIconPixmap(entry->database(), state.uuid, IconSize::Medium);
        } else if (state.uuid.isNull() && state.number >= 0) {
            pixmap = databaseIcons()->icon(state.number, IconSize::Medium);
        }
    }
    if (pixmap.isNull() && entry) {
        pixmap = Icons::entryIconPixmap(entry, IconSize::Medium);
    }
    static_cast<IconTile*>(m_iconTile)->setPixmap(pixmap);
    m_iconTile->setEnabled(!readOnly());

    // The editor's headline starts with the group, as in "Work • GitHub • Edit entry"
    const auto group = m_headline->text().section(QStringLiteral(" • "), 0, 0);
    if (readOnly()) {
        m_subtitle->setText(tr("%1 · Earlier version, read-only").arg(group));
    } else if (m_create) {
        m_subtitle->setText(tr("New in %1").arg(group));
    } else {
        m_subtitle->setText(tr("Editing in %1").arg(group));
    }
}

void VaultEntryEditCard::suggestTitleFromUrl(const QString& url)
{
    // A new entry takes its title from the site until the user types one of their own
    if (!m_create || !m_titleEdit) {
        return;
    }
    const auto title = m_titleEdit->text();
    if (!title.isEmpty() && title != m_suggestedTitle) {
        return;
    }
    auto host = QUrl::fromUserInput(url.trimmed()).host();
    if (host.startsWith("www.")) {
        host = host.mid(4);
    }
    m_suggestedTitle = host;
    m_titleEdit->setText(host);
}

void VaultEntryEditCard::applyColors()
{
    // Style sheets polish the widgets again and reset their palettes, so set the label colors after
    m_editor->setStyleSheet(Vault::formStyleSheet());
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_pageTitle, Typography::FontRole::BodyLargeStrong, p.text);
    Vault::styleLabel(m_subtitle, Typography::FontRole::Caption, p.textSecondary);
    for (auto* heading : m_headings) {
        Vault::styleHeading(heading, 17, p.text);
    }
    styleRowLabels();
}

// Colors only the rows' labels; setting the form's style sheet again would repolish every page
void VaultEntryEditCard::styleRowLabels()
{
    const auto p = FluentTheme::palette();
    m_captions.removeAll(nullptr);
    m_notes.removeAll(nullptr);
    for (const auto& caption : m_captions) {
        Vault::styleLabel(caption, Typography::FontRole::Body, p.textSecondary);
    }
    for (const auto& note : m_notes) {
        Vault::styleLabel(note, Typography::FontRole::Caption, p.textSecondary);
    }
}

bool VaultEntryEditCard::isFollowing() const
{
    return !m_following.isEmpty();
}

VaultDimmer::VaultDimmer(QWidget* pane, std::function<QColor()> color)
    : QWidget(pane)
    , m_color(std::move(color))
{
    setObjectName("vaultDimmer");
    pane->installEventFilter(this);
    setGeometry(pane->rect());
    raise();
    Vault::followTheme(this, [this] { update(); });
}

bool VaultDimmer::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void VaultDimmer::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    auto color = m_color();
    // High contrast has no translucent fills: hide the panes instead of fading them
    color.setAlphaF(FluentTheme::isHighContrast() ? 1.0 : 0.55);
    QPainter(this).fillRect(rect(), color);
}
