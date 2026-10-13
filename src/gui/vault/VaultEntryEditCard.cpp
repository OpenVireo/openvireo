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

#include "VaultEntryEditCard.h"

#include "core/Entry.h"
#include "gui/CategoryListWidget.h"
#include "gui/DatabaseIcons.h"
#include "gui/EditWidget.h"
#include "gui/EditWidgetIcons.h"
#include "gui/Icons.h"
#include "gui/entry/EditEntryWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/tag/TagsEdit.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultEditShell.h"
#include "gui/vault/VaultWidgets.h"

#include <components/textfields/LineEdit.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    constexpr int TileSize = 44;
    constexpr int SectionRowHeight = 52;
    constexpr int GroupPadding = 14;
    constexpr int FieldGap = 12;
    constexpr int GroupGap = 18;
    constexpr int NotesMinHeight = 96;
    constexpr int NotesMaxHeight = 160;

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
            path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
            painter.fillPath(path, p.inset);
            if (isEnabled() && (isDown() || underMouse())) {
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
            }
            painter.setPen(QPen(hasFocus() ? p.text : p.stroke, hasFocus() ? 2 : 1));
            painter.drawPath(path);
            if (!m_pixmap.isNull()) {
                const int size = 24;
                painter.drawPixmap(QRect((width() - size) / 2, (height() - size) / 2, size, size), m_pixmap);
            }
        }

    private:
        QPixmap m_pixmap;
    };

    // Rounded surface one step quieter than the card, holding fields or rows, as the detail card groups its fields
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
            path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            painter.fillPath(path, p.inset);
            painter.setPen(QPen(p.stroke, 1));
            painter.drawPath(path);
            if (!m_dividers) {
                return;
            }
            // Dividers between visible rows
            painter.setPen(QPen(p.divider, 1));
            bool first = true;
            for (auto* child : findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
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
                path.addRoundedRect(row, 4, 4);
                painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
            }
            if (hasFocus()) {
                QPainterPath path;
                path.addRoundedRect(row.adjusted(1, 1, -1, -1), 4, 4);
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
            {"Advanced", Typography::Icons::Settings, VaultEntryEditCard::tr("Attributes, attachments and colors")},
            {"Icon", Typography::Icons::Emoji, VaultEntryEditCard::tr("Choose an icon or use the site's")},
            {"Auto-Type", Typography::Icons::Keyboard, VaultEntryEditCard::tr("Sequences and window associations")},
            {"Browser Integration", Typography::Icons::World, VaultEntryEditCard::tr("Site access and extra URLs")},
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

    // Caption of a form label without its trailing colon, read above its field
    void makeCaption(QLabel* label)
    {
        auto text = label->text().trimmed();
        while (text.endsWith(QLatin1Char(':')) || text.endsWith(QChar(0xff1a))) {
            text.chop(1);
        }
        label->setText(text);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
} // namespace

VaultEntryEditCard::VaultEntryEditCard(EditWidget* editor, QWidget* parent)
    : fluent::layout::Card(parent)
    , m_editor(editor)
    , m_categories(editor->categoryList())
    , m_pages(editor->pageStack())
    , m_headline(editor->headlineLabel())
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

    // The pages are reached from the rows under the Entry page instead of a category list
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

    clearPageBackgrounds(m_pages);
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

// Fields with their captions above them, grouped as on the detail card, and the other pages listed beneath
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

    // A caption above its field, or above a row of controls
    auto field = [this](QWidget* parent, QLabel* caption, const QWidgetList& controls, int stretchIndex = 0) {
        auto* block = new QWidget(parent);
        auto* blockLayout = new QVBoxLayout(block);
        blockLayout->setContentsMargins(0, 0, 0, 0);
        blockLayout->setSpacing(4);
        if (caption) {
            makeCaption(caption);
            adopt(caption, block);
            blockLayout->addWidget(caption);
            m_captions << caption;
        }
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        for (int i = 0; i < controls.size(); ++i) {
            adopt(controls[i], block);
            row->addWidget(controls[i], i == stretchIndex ? 1 : 0);
        }
        blockLayout->addLayout(row);
        return block;
    };
    auto group = [](QWidget* parent) {
        auto* box = new InsetGroup(parent, false);
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(GroupPadding, GroupPadding - 2, GroupPadding, GroupPadding);
        boxLayout->setSpacing(FieldGap);
        return box;
    };

    // Sign-in: username, password and site
    auto* signIn = group(body);
    signIn->setObjectName("vaultEditSignIn");
    signIn->layout()->addWidget(field(signIn, label("usernameLabel"), {find("usernameComboBox")}));
    signIn->layout()->addWidget(field(signIn, label("passwordLabel"), {find("passwordEdit")}));
    signIn->layout()->addWidget(field(signIn, label("urlLabel"), {find("urlEdit"), find("fetchFaviconButton")}));
    layout->addWidget(signIn);

    // Notes, with the button that reveals them beside their caption, at a height that leaves the rest in view
    auto* notesBlock = new QWidget(body);
    notesBlock->setObjectName("vaultEditNotes");
    auto* notesLayout = new QVBoxLayout(notesBlock);
    notesLayout->setContentsMargins(0, 0, 0, 0);
    notesLayout->setSpacing(4);
    auto* notesHeader = new QHBoxLayout();
    notesHeader->setContentsMargins(0, 0, 0, 0);
    auto* notesLabel = label("notesLabel");
    makeCaption(notesLabel);
    adopt(notesLabel, notesBlock);
    m_captions << notesLabel;
    notesHeader->addWidget(notesLabel);
    notesHeader->addStretch();
    auto* revealNotes = find("revealNotesButton");
    adopt(revealNotes, notesBlock);
    notesHeader->addWidget(revealNotes);
    notesLayout->addLayout(notesHeader);
    auto* notes = find("notesEdit");
    adopt(notes, notesBlock);
    notes->setMinimumHeight(NotesMinHeight);
    notes->setMaximumHeight(NotesMaxHeight);
    notes->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    notesLayout->addWidget(notes);
    layout->addWidget(notesBlock);

    // Tags and expiry
    auto* details = group(body);
    details->setObjectName("vaultEditDetails");
    m_detailsGroup = details;
    auto* tagsBlock = field(details, label("tagsLabel"), {find("tagsList")});
    details->layout()->addWidget(tagsBlock);
    // The check box says what it does, so the row needs no caption of its own
    auto* expireCheck = qobject_cast<QAbstractButton*>(find("expireCheck"));
    if (auto* expireLabel = label("expireLabel")) {
        makeCaption(expireLabel);
        expireCheck->setText(VaultButton::withoutMnemonic(expireLabel->text()));
        expireLabel->hide();
    }
    auto* expiryBlock = field(details, nullptr, {expireCheck, find("expireDatePicker"), find("expirePresets")}, 1);
    details->layout()->addWidget(expiryBlock);
    layout->addWidget(details);

    // Chips that bring back the blocks a new entry starts without
    m_optionalRows = {
        {tr("Notes"), notesBlock, "notesEdit"},
        {tr("Tags"), tagsBlock, "tagsList"},
        {tr("Expiry"), expiryBlock, "expireCheck"},
    };
    m_addRow = new QWidget(body);
    m_addRow->setObjectName("vaultEditAddRow");
    m_addLayout = new QHBoxLayout(m_addRow);
    m_addLayout->setContentsMargins(0, 0, 0, 0);
    m_addLayout->setSpacing(8);
    m_addLabel = new QLabel(tr("Add"), m_addRow);
    m_addLayout->addWidget(m_addLabel);
    for (int i = 0; i < m_optionalRows.size(); ++i) {
        auto* chip = new fluent::basicinput::Button(m_optionalRows[i].chipText, m_addRow);
        chip->setObjectName("vaultEditAdd" + m_optionalRows[i].focusName);
        chip->setFluentLayout(fluent::basicinput::Button::IconBefore);
        chip->setFluentSize(fluent::basicinput::Button::Small);
        chip->setIconGlyph(Typography::Icons::Add, Typography::IconSize::Compact);
        connect(chip, &QAbstractButton::clicked, this, [this, i] { showOptionalRow(i, true); });
        m_addLayout->addWidget(chip);
        m_chips << chip;
    }
    m_addLayout->addStretch();
    m_addRow->hide();
    layout->addWidget(m_addRow);

    // The other pages, each with what it holds
    auto* more = new QWidget(body);
    auto* moreLayout = new QVBoxLayout(more);
    moreLayout->setContentsMargins(0, 0, 0, 0);
    moreLayout->setSpacing(6);
    m_moreLabel = new QLabel(tr("More"), more);
    moreLayout->addWidget(m_moreLabel);
    m_sections = new InsetGroup(more, true);
    m_sections->setObjectName("vaultEditSections");
    m_sectionsLayout = new QVBoxLayout(m_sections);
    m_sectionsLayout->setContentsMargins(0, 0, 0, 0);
    m_sectionsLayout->setSpacing(0);
    moreLayout->addWidget(m_sections);
    layout->addWidget(more);
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

void VaultEntryEditCard::prepare(bool create)
{
    m_create = create;
    m_suggestedTitle.clear();

    finish();
    if (create) {
        for (int i = 0; i < m_optionalRows.size(); ++i) {
            if (isOptionalRowEmpty(i)) {
                m_optionalRows[i].block->hide();
                m_tucked.insert(i);
                m_chips[i]->show();
            }
        }
    }
    m_addRow->setVisible(!m_tucked.isEmpty());
    updateDetailsGroup();

    rebuildSections();
    updateIdentity();
    pageChanged(m_pages->currentIndex());
    if (m_titleEdit && !m_editor->readOnly()) {
        QTimer::singleShot(0, m_titleEdit, [title = m_titleEdit] { title->setFocus(); });
    }
}

void VaultEntryEditCard::finish()
{
    for (int i = 0; i < m_optionalRows.size(); ++i) {
        showOptionalRow(i, false);
    }
}

void VaultEntryEditCard::setInModal(bool inModal)
{
    m_inModal = inModal;
    // The modal's footer has the buttons and its title says what is being made, so the header keeps only
    // the icon and the title field
    m_subtitle->setVisible(!inModal);
    update();
}

bool VaultEntryEditCard::isInModal() const
{
    return m_inModal;
}

void VaultEntryEditCard::paintEvent(QPaintEvent* event)
{
    // The modal's card is the surface
    if (!m_inModal) {
        fluent::layout::Card::paintEvent(event);
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
    m_sections->parentWidget()->setVisible(m_sectionsLayout->count() > 0);
}

void VaultEntryEditCard::pageChanged(int index)
{
    m_backRow->setVisible(index > 0);
    auto* item = m_categories->listWidget()->item(index);
    m_pageTitle->setText(item ? item->text() : QString());
}

void VaultEntryEditCard::showOptionalRow(int row, bool focus)
{
    if (m_tucked.remove(row)) {
        m_optionalRows[row].block->show();
    }
    m_chips[row]->hide();
    m_addRow->setVisible(!m_tucked.isEmpty());
    updateDetailsGroup();
    if (focus) {
        if (auto* widget = m_optionalRows[row].block->findChild<QWidget*>(m_optionalRows[row].focusName)) {
            widget->setFocus();
        }
    }
}

bool VaultEntryEditCard::isOptionalRowEmpty(int row) const
{
    auto* page = m_pages->widget(0);
    const auto& focusName = m_optionalRows[row].focusName;
    if (auto* notes = page->findChild<QPlainTextEdit*>(focusName)) {
        return notes->toPlainText().isEmpty();
    }
    if (auto* tags = page->findChild<TagsEdit*>(focusName)) {
        return tags->tags().isEmpty();
    }
    if (auto* check = page->findChild<QCheckBox*>(focusName)) {
        return !check->isChecked();
    }
    return false;
}

void VaultEntryEditCard::updateDetailsGroup()
{
    // The tags and expiry group goes while both of its blocks wait behind chips
    if (m_detailsGroup) {
        m_detailsGroup->setVisible(!m_tucked.contains(1) || !m_tucked.contains(2));
    }
}

// The icon the entry has, or the one picked on the Icon page, and where the entry lives
void VaultEntryEditCard::updateIdentity()
{
    auto* entryEditor = qobject_cast<EditEntryWidget*>(m_editor);
    auto* entry = entryEditor ? entryEditor->currentEntry() : nullptr;
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
    m_iconTile->setEnabled(!m_editor->readOnly());

    // The editor's headline starts with the group, as in "Work • GitHub • Edit entry"
    const auto group = m_headline->text().section(QStringLiteral(" • "), 0, 0);
    if (m_editor->readOnly()) {
        m_subtitle->setText(tr("%1 · Earlier version, read-only").arg(group));
    } else if (m_create) {
        m_subtitle->setText(tr("New entry in %1").arg(group));
    } else {
        m_subtitle->setText(tr("%1 · Editing").arg(group));
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
    Vault::styleLabel(m_addLabel, Typography::FontRole::Caption, p.textSecondary);
    if (m_moreLabel) {
        Vault::styleLabel(m_moreLabel, Typography::FontRole::BodyStrong, p.text);
    }
    for (auto* caption : m_captions) {
        Vault::styleLabel(caption, Typography::FontRole::Caption, p.textSecondary);
    }
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
    color.setAlphaF(0.55);
    QPainter(this).fillRect(rect(), color);
}
