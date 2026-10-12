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

#include "gui/CategoryListWidget.h"
#include "gui/EditWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/tag/TagsEdit.h"
#include "gui/vault/VaultEditShell.h"
#include "gui/vault/VaultWidgets.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    constexpr int SectionRowHeight = 40;
    constexpr int DraftRowHeight = 52;
    constexpr int DraftTileSize = 32;

    // One of the editor's other pages, listed under the Entry page: its name and a chevron
    class SectionRow : public QAbstractButton
    {
    public:
        SectionRow(const QString& text, QWidget* parent)
            : QAbstractButton(parent)
        {
            setText(text);
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
            setAttribute(Qt::WA_Hover);
        }

        QSize sizeHint() const override
        {
            return {200, SectionRowHeight};
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);

            painter.setPen(QPen(p.divider, 1));
            painter.drawLine(QPointF(0, 0.5), QPointF(width(), 0.5));

            const QRectF row = QRectF(rect()).adjusted(0, 2, 0, -1);
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

            painter.setFont(Vault::font(Typography::FontRole::Body));
            painter.setPen(p.text);
            painter.drawText(row.adjusted(8, 0, -32, 0), Qt::AlignLeft | Qt::AlignVCenter, text());

            QFont iconFont(Typography::FontFamily::FluentIcons);
            iconFont.setPixelSize(Typography::IconSize::Compact);
            painter.setFont(iconFont);
            painter.setPen(p.textSecondary);
            painter.drawText(QRectF(row.right() - 28, row.top(), 20, row.height()),
                             Qt::AlignCenter,
                             Typography::Icons::ChevronRight);
        }
    };

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

    // The form inside the Entry page, where the section rows are added below the fields
    QWidget* entryPageForm(QStackedWidget* pages)
    {
        auto* scroll = qobject_cast<QScrollArea*>(pages->widget(0));
        return scroll ? scroll->widget() : nullptr;
    }

    void addBelowForm(QWidget* form, QWidget* widget)
    {
        if (auto* grid = qobject_cast<QGridLayout*>(form->layout())) {
            grid->addWidget(widget, grid->rowCount(), 0, 1, grid->columnCount());
        } else if (auto* box = qobject_cast<QBoxLayout*>(form->layout())) {
            box->addWidget(widget);
        }
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
        layout->setContentsMargins(18, 14, 18, 14);
        layout->setSpacing(10);
    }

    // The pages are reached from the rows under the Entry page instead of a category list
    m_categories->hide();

    // Header: the headline, the focus mode toggle, then Cancel and Save
    auto* header = new QWidget(editor);
    header->setObjectName("vaultEditCardHeader");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 4);
    headerLayout->setSpacing(8);

    // Leave an empty, hidden stand-in where the headline was so the form's layout keeps its shape
    auto* placeholder = new QWidget();
    placeholder->hide();
    m_headline->parentWidget()->layout()->replaceWidget(m_headline, placeholder);
    m_headline->setParent(header);
    m_headline->setContentsMargins(0, 0, 0, 0);
    m_headline->setWordWrap(true);
    headerLayout->addWidget(m_headline, 1);

    m_focusButton = Vault::iconButton(Typography::Icons::FullScreen, tr("Expand editor"), header);
    m_focusButton->setObjectName("vaultEditFocusButton");
    connect(m_focusButton, &QAbstractButton::clicked, this, [this] { emit focusModeRequested(!m_focusMode); });
    headerLayout->addWidget(m_focusButton, 0, Qt::AlignTop);

    m_buttonBar = new VaultButtonBar(editor->buttonBox(), header);
    m_buttonBar->setButtonText(QDialogButtonBox::Ok, tr("Save"));
    // Save returns to the entry's details, so there is no need to apply and stay
    m_buttonBar->setButtonHidden(QDialogButtonBox::Apply, true);
    headerLayout->addWidget(m_buttonBar, 0, Qt::AlignTop);
    qobject_cast<QBoxLayout*>(editor->layout())->insertWidget(0, header);

    // Back row above the other pages, which open in place of the Entry page
    auto* pageHost = new QWidget(editor);
    auto* pageHostLayout = new QVBoxLayout(pageHost);
    pageHostLayout->setContentsMargins(0, 0, 0, 0);
    pageHostLayout->setSpacing(8);
    m_backRow = new QWidget(pageHost);
    auto* backLayout = new QHBoxLayout(m_backRow);
    backLayout->setContentsMargins(0, 0, 0, 0);
    backLayout->setSpacing(8);
    auto* backButton = new fluent::basicinput::Button(QString(), m_backRow);
    backButton->setObjectName("vaultEditBackButton");
    backButton->setFluentStyle(fluent::basicinput::Button::Subtle);
    backButton->setFluentLayout(fluent::basicinput::Button::IconBefore);
    backButton->setFluentSize(fluent::basicinput::Button::Small);
    backButton->setIconGlyph(Typography::Icons::ChevronLeft, Typography::IconSize::Compact);
    if (m_categories->listWidget()->count() > 0) {
        backButton->setText(m_categories->listWidget()->item(0)->text());
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

    auto* form = entryPageForm(m_pages);

    // Chips that bring back the rows a new entry starts without
    m_optionalRows = {
        {tr("Notes"), {"notesLabel", "revealNotesButton", "notesEdit"}, "notesEdit"},
        {tr("Tags"), {"tagsLabel", "tagsList"}, "tagsList"},
        {tr("Expiry"), {"expireLabel", "expireCheck", "expireDatePicker", "expirePresets"}, "expireCheck"},
    };
    m_addRow = new QWidget(form);
    m_addRow->setObjectName("vaultEditAddRow");
    m_addLayout = new QHBoxLayout(m_addRow);
    m_addLayout->setContentsMargins(0, 6, 0, 0);
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

    m_sections = new QWidget(form);
    m_sections->setObjectName("vaultEditSections");
    m_sectionsLayout = new QVBoxLayout(m_sections);
    m_sectionsLayout->setContentsMargins(0, 10, 0, 0);
    m_sectionsLayout->setSpacing(0);

    if (form) {
        addBelowForm(form, m_addRow);
        addBelowForm(form, m_sections);
        m_titleEdit = form->findChild<QLineEdit*>("titleEdit");
        if (auto* urlEdit = form->findChild<QLineEdit*>("urlEdit")) {
            connect(urlEdit, &QLineEdit::textEdited, this, &VaultEntryEditCard::suggestTitleFromUrl);
        }
    }

    clearPageBackgrounds(m_pages);
    pageChanged(m_pages->currentIndex());
    applyColors();
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
            if (!isOptionalRowEmpty(i)) {
                continue;
            }
            auto* page = m_pages->widget(0);
            for (const auto& name : m_optionalRows[i].widgetNames) {
                auto* widget = page->findChild<QWidget*>(name);
                if (!widget) {
                    continue;
                }
                m_hiddenBefore.insert(widget, widget->isHidden());
                widget->hide();
                // Spacers sharing a layout with the row's widgets would otherwise hold its height
                for (auto* layout : page->findChildren<QLayout*>()) {
                    if (layout->indexOf(widget) < 0) {
                        continue;
                    }
                    for (int j = 0; j < layout->count(); ++j) {
                        auto* spacer = layout->itemAt(j)->spacerItem();
                        if (spacer && !spacer->sizeHint().isEmpty()) {
                            m_collapsedSpacers[i].append({spacer, spacer->sizeHint(), spacer->sizePolicy()});
                            spacer->changeSize(0, 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
                            layout->invalidate();
                        }
                    }
                }
            }
            m_chips[i]->show();
        }
    }
    m_addRow->setVisible(create && !m_hiddenBefore.isEmpty());

    rebuildSections();
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
    setFocusMode(false);
}

void VaultEntryEditCard::setFocusMode(bool focusMode)
{
    m_focusMode = focusMode;
    m_focusButton->setIconGlyph(focusMode ? Typography::Icons::BackToWindow : Typography::Icons::FullScreen);
    m_focusButton->setToolTip(focusMode ? tr("Show the entry list") : tr("Expand editor"));
    m_focusButton->setAccessibleName(m_focusButton->toolTip());
}

bool VaultEntryEditCard::isFocusMode() const
{
    return m_focusMode;
}

void VaultEntryEditCard::onThemeUpdated()
{
    fluent::layout::Card::onThemeUpdated();
    applyColors();
}

void VaultEntryEditCard::rebuildSections()
{
    while (auto* item = m_sectionsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto* list = m_categories->listWidget();
    for (int i = 1; i < list->count(); ++i) {
        if (m_categories->isCategoryHidden(i)) {
            continue;
        }
        auto* row = new SectionRow(list->item(i)->text(), m_sections);
        connect(row, &QAbstractButton::clicked, this, [this, i] { m_categories->setCurrentCategory(i); });
        m_sectionsLayout->addWidget(row);
    }
    m_sections->setVisible(m_sectionsLayout->count() > 0);
}

void VaultEntryEditCard::pageChanged(int index)
{
    m_backRow->setVisible(index > 0);
    auto* item = m_categories->listWidget()->item(index);
    m_pageTitle->setText(item ? item->text() : QString());
}

void VaultEntryEditCard::showOptionalRow(int row, bool focus)
{
    QWidget* focusWidget = nullptr;
    for (const auto& name : m_optionalRows[row].widgetNames) {
        auto* widget = m_pages->widget(0)->findChild<QWidget*>(name);
        if (widget && m_hiddenBefore.contains(widget)) {
            widget->setHidden(m_hiddenBefore.take(widget));
        }
        if (name == m_optionalRows[row].focusName) {
            focusWidget = widget;
        }
    }
    for (const auto& collapsed : m_collapsedSpacers.take(row)) {
        collapsed.spacer->changeSize(collapsed.size.width(),
                                     collapsed.size.height(),
                                     collapsed.policy.horizontalPolicy(),
                                     collapsed.policy.verticalPolicy());
    }
    if (auto* layout = m_pages->widget(0)->layout()) {
        layout->invalidate();
    }
    m_chips[row]->hide();
    m_addRow->setVisible(m_create && !m_hiddenBefore.isEmpty());
    if (focus && focusWidget) {
        focusWidget->setFocus();
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
    Vault::styleLabel(m_headline, Typography::FontRole::Subtitle, p.text);
    Vault::styleLabel(m_pageTitle, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_addLabel, Typography::FontRole::Caption, p.textSecondary);
}

VaultDraftRow::VaultDraftRow(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("vaultDraftRow");
    setTitle(QString());
    Vault::followTheme(this, [this] { update(); });
}

void VaultDraftRow::setTitle(const QString& title)
{
    m_title = title.trimmed().isEmpty() ? tr("New entry") : title.trimmed();
    setAccessibleName(tr("%1, not saved yet").arg(m_title));
    update();
}

QString VaultDraftRow::title() const
{
    return m_title;
}

QSize VaultDraftRow::sizeHint() const
{
    return {200, DraftRowHeight};
}

void VaultDraftRow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Selected like a list row, outlined in the accent while it is a draft
    const QRectF row = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath path;
    path.addRoundedRect(row, 6, 6);
    painter.fillPath(path, p.layer);
    painter.setPen(QPen(p.accent, 1.5));
    painter.drawPath(path);

    const QRectF tile(row.left() + 11, row.center().y() - DraftTileSize / 2.0, DraftTileSize, DraftTileSize);
    QPainterPath tilePath;
    tilePath.addRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
    painter.fillPath(tilePath, p.accentSubtle);
    painter.setPen(QPen(p.accent, 1, Qt::DashLine));
    painter.drawPath(tilePath);

    QFont iconFont(Typography::FontFamily::FluentIcons);
    iconFont.setPixelSize(Typography::IconSize::Compact);
    painter.setFont(iconFont);
    painter.setPen(p.accentText);
    painter.drawText(tile, Qt::AlignCenter, Typography::Icons::Add);

    const qreal textLeft = tile.right() + 12;
    const QRectF textRect(textLeft, row.top(), row.right() - textLeft - 12, row.height());
    const auto titleFont = Vault::font(Typography::FontRole::BodyStrong);
    const auto captionFont = Vault::font(Typography::FontRole::Caption);
    const int titleHeight = QFontMetrics(titleFont).height();
    const int captionHeight = QFontMetrics(captionFont).height();
    const qreal top = row.center().y() - (titleHeight + captionHeight) / 2.0;

    painter.setFont(titleFont);
    painter.setPen(p.text);
    painter.drawText(QRectF(textRect.left(), top, textRect.width(), titleHeight),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(titleFont).elidedText(m_title, Qt::ElideRight, int(textRect.width())));
    painter.setFont(captionFont);
    painter.setPen(p.textSecondary);
    painter.drawText(QRectF(textRect.left(), top + titleHeight, textRect.width(), captionHeight),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     tr("Draft · not saved yet"));
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
