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

#ifndef OPENVIREO_VAULTENTRYEDITCARD_H
#define OPENVIREO_VAULTENTRYEDITCARD_H

#include <QPointer>
#include <QSet>

#include <functional>

#include <components/layout/Card.h>

class CategoryListWidget;
class EditEntryWidget;
class EditWidget;
class EntryAttachments;
class EntryAttributes;
class QAbstractButton;
class QButtonGroup;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class VaultButtonBar;

namespace fluent::basicinput
{
    class Button;
}

/**
 * Presents an entry editor in the detail pane, in place of the entry's detail card, for new entries and
 * existing ones alike, so the groups and the entry list stay in view while editing.
 *
 * The card reads as the detail card made editable (dev guide plans/entry-editor.md): the entry's icon and its
 * title as a large field, then rows with the caption on the left and the field beside it (above it when the
 * card is narrow), in the detail card's groups: sign-in (username, password, one-time code, URLs), additional
 * attributes, details (tags, expiry, attachments) and notes. One "Add field" menu adds a one-time code, another
 * URL, an attribute or an attachment. A new entry starts from a type (login, note, card...), which only picks
 * the fields it starts with. The editor's other pages wait under a collapsed "More options".
 *
 * Fields that the old editor keeps on other pages (attributes, extra URLs, the one-time code, attachments) are
 * edited here in the editor's working copy, so saving works as before.
 */
class VaultEntryEditCard : public fluent::layout::Card
{
    Q_OBJECT

public:
    explicit VaultEntryEditCard(EditWidget* editor, QWidget* parent = nullptr);

    EditWidget* editor() const;
    // Set up for the entry the editor has just loaded
    void prepare(bool create);
    // Stop following the entry, once editing ends
    void finish();
    void onThemeUpdated() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildHeader();
    void buildEntryPage();
    void rebuildSections();
    void pageChanged(int index);
    void updateIdentity();
    void suggestTitleFromUrl(const QString& url);
    void applyColors();
    void styleRowLabels();
    bool isFollowing() const;

    // The rows that follow the editor's working attributes and attachments
    void scheduleRebuild();
    void rebuildDynamicRows();
    void rebuildSignInRows();
    void rebuildAttributeRows();
    void rebuildAttachmentRows();
    void updateTotpCode();
    void normalizeTotp();
    QString totpKey() const;

    void applyType(const QString& type);
    void populateAddMenu();
    void addAttribute(bool hidden);
    void addUrl();
    void addTotp();
    void setExpiry(int choice);
    void updateExpiryNote();
    void setStacked(bool stacked);
    void focusLater(QWidget* widget);

    EditEntryWidget* entryEditor() const;
    bool readOnly() const;

    QPointer<EditWidget> m_editor;
    CategoryListWidget* m_categories;
    QStackedWidget* m_pages;
    QWidget* m_header;
    QLabel* m_headline;
    VaultButtonBar* m_buttonBar;
    QAbstractButton* m_iconTile = nullptr;
    QLabel* m_subtitle = nullptr;

    QWidget* m_backRow;
    QLabel* m_pageTitle;

    // Entry page
    QWidget* m_typeRow = nullptr;
    QList<QAbstractButton*> m_typeChips;
    QString m_type;
    QStringList m_seededKeys;
    QSet<QString> m_hiddenFields;

    QWidget* m_signIn = nullptr;
    QVBoxLayout* m_signInLayout = nullptr;
    QWidget* m_userRow = nullptr;
    QWidget* m_passwordRow = nullptr;
    QWidget* m_urlRow = nullptr;
    QList<QWidget*> m_signInExtras;
    QLineEdit* m_totpEdit = nullptr;
    QLabel* m_totpCode = nullptr;
    QTimer* m_totpTimer;

    QLabel* m_attributesHeading = nullptr;
    QWidget* m_attributesGroup = nullptr;
    QVBoxLayout* m_attributesLayout = nullptr;

    QWidget* m_detailsGroup = nullptr;
    QVBoxLayout* m_detailsLayout = nullptr;
    QList<QWidget*> m_attachmentRows;
    QButtonGroup* m_expiryChoices = nullptr;
    QLabel* m_expiryNote = nullptr;

    QAbstractButton* m_addButton = nullptr;
    QAbstractButton* m_moreToggle = nullptr;
    QWidget* m_sections = nullptr;
    QVBoxLayout* m_sectionsLayout = nullptr;

    // Labels of rows that are built again go with their rows, so these are guarded
    QList<QPointer<QLabel>> m_captions;
    QList<QLabel*> m_headings;
    QList<QPointer<QLabel>> m_notes;

    QPointer<EntryAttributes> m_attributes;
    QPointer<EntryAttachments> m_attachments;
    QList<QMetaObject::Connection> m_following;
    bool m_rebuildPending = false;
    // Keeps the caret in the row being edited when the rows are built again
    QString m_focusKey;
    QString m_focusPart;

    QPointer<QLineEdit> m_titleEdit;
    QString m_suggestedTitle;
    bool m_create = false;
    bool m_stacked = false;
};

/**
 * Translucent cover over a pane that waits while an entry is edited, in the colour of the
 * surface beneath so the pane reads as set back. It follows the pane's size and takes its clicks.
 */
class VaultDimmer : public QWidget
{
    Q_OBJECT

public:
    VaultDimmer(QWidget* pane, std::function<QColor()> color);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    std::function<QColor()> m_color;
};

#endif // OPENVIREO_VAULTENTRYEDITCARD_H
