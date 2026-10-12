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

#include <QHash>
#include <QPointer>
#include <QSizePolicy>

#include <functional>

#include <components/layout/Card.h>

class CategoryListWidget;
class EditWidget;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QSpacerItem;
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
 * The card shows the editor's headline, with Cancel and Save in the command bar, then the Entry page with
 * the other pages listed beneath it as rows; a row opens its page in place, with a way back to the Entry
 * page. New entries start compact: notes, tags and expiry wait behind "add" chips until asked for.
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
    // In a modal, which has its own title and buttons, the card drops its headline, buttons and surface
    void setInModal(bool inModal);
    bool isInModal() const;
    void onThemeUpdated() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct OptionalRow
    {
        QString chipText;
        QStringList widgetNames;
        QString focusName;
    };

    void rebuildSections();
    void pageChanged(int index);
    void showOptionalRow(int row, bool focus);
    bool isOptionalRowEmpty(int row) const;
    void suggestTitleFromUrl(const QString& url);
    void applyColors();

    QPointer<EditWidget> m_editor;
    CategoryListWidget* m_categories;
    QStackedWidget* m_pages;
    QWidget* m_header;
    QLabel* m_headline;
    VaultButtonBar* m_buttonBar;

    QWidget* m_backRow;
    QLabel* m_pageTitle;
    QWidget* m_sections;
    QVBoxLayout* m_sectionsLayout;
    QWidget* m_addRow;
    QHBoxLayout* m_addLayout;
    QLabel* m_addLabel;

    QList<OptionalRow> m_optionalRows;
    QList<fluent::basicinput::Button*> m_chips;
    // Visibility the editor gave each widget of a row before the card tucked it away
    QHash<QWidget*, bool> m_hiddenBefore;
    // Spacers beside a tucked-away row, collapsed so the row leaves no gap, with the size they had
    struct CollapsedSpacer
    {
        QSpacerItem* spacer;
        QSize size;
        QSizePolicy policy;
    };
    QHash<int, QList<CollapsedSpacer>> m_collapsedSpacers;

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
