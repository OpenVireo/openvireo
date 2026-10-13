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

#ifndef KEEPASSXC_VAULTENTRYEDITCARD_H
#define KEEPASSXC_VAULTENTRYEDITCARD_H

#include <QPointer>
#include <QSet>

#include <functional>

#include <components/layout/Card.h>

class CategoryListWidget;
class QAbstractButton;
class EditWidget;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QVBoxLayout;
class VaultButtonBar;

namespace fluent::basicinput
{
    class Button;
}

/**
 * Presents an entry editor in the Vault Index detail pane, in place of the entry's detail card, so the
 * groups and the entry list stay in view while editing.
 *
 * The card reads as the detail card made editable: the entry's icon and its title as a large field at the
 * top, then the fields with their captions above them, grouped like the detail card's rows. The other pages
 * are listed under a "More" heading, each with what it holds; a row opens its page in place, with a way
 * back to the Entry page. Cancel and Save sit in the command bar. New entries start compact: notes, tags
 * and expiry wait behind "add" chips until asked for.
 *
 * Only the presentation changes; the editor's widgets and logic are untouched.
 */
class VaultEntryEditCard : public fluent::layout::Card
{
    Q_OBJECT

public:
    explicit VaultEntryEditCard(EditWidget* editor, QWidget* parent = nullptr);

    EditWidget* editor() const;
    // Set up for the entry the editor has just loaded
    void prepare(bool create);
    // Leave the form as the editor laid it out, once editing ends
    void finish();
    // In a modal, which has its own title and buttons, the card drops its surface
    void setInModal(bool inModal);
    bool isInModal() const;
    void onThemeUpdated() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct OptionalRow
    {
        QString chipText;
        QWidget* block;
        QString focusName;
    };

    void buildHeader();
    void buildEntryPage();
    void rebuildSections();
    void pageChanged(int index);
    void showOptionalRow(int row, bool focus);
    bool isOptionalRowEmpty(int row) const;
    void updateDetailsGroup();
    void updateIdentity();
    void suggestTitleFromUrl(const QString& url);
    void applyColors();

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
    QWidget* m_sections = nullptr;
    QVBoxLayout* m_sectionsLayout = nullptr;
    QLabel* m_moreLabel = nullptr;
    QWidget* m_detailsGroup = nullptr;
    QWidget* m_addRow;
    QHBoxLayout* m_addLayout;
    QLabel* m_addLabel;
    QList<QLabel*> m_captions;

    QList<OptionalRow> m_optionalRows;
    QList<fluent::basicinput::Button*> m_chips;
    // Blocks the card tucked away for a new entry
    QSet<int> m_tucked;

    QPointer<QLineEdit> m_titleEdit;
    QString m_suggestedTitle;
    bool m_create = false;
    bool m_inModal = false;
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

#endif // KEEPASSXC_VAULTENTRYEDITCARD_H
