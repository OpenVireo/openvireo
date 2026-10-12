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

#ifndef KEEPASSXC_ENTRYDETAILCARD_H
#define KEEPASSXC_ENTRYDETAILCARD_H

#include <QPointer>

#include <components/layout/Card.h>

class Entry;
class Group;
class QLabel;
class QStackedLayout;
class QTimer;
class QVBoxLayout;
class VaultFieldRow;

namespace fluent::basicinput
{
    class Button;
}

/**
 * Vault Index detail pane: the selected entry with one row per field and a
 * copy, reveal or open button on each row. Shows an empty state for groups.
 */
class EntryDetailCard : public fluent::layout::Card
{
    Q_OBJECT

public:
    explicit EntryDetailCard(QWidget* parent = nullptr);

    void setEntry(Entry* entry);
    void setGroup(Group* group);
    void refresh();
    void onThemeUpdated() override;

private:
    void updateHeader();
    void updateFields();
    void updateTotp();
    void updateMeta();
    void applyColors();

    QPointer<Entry> m_entry;
    QPointer<Group> m_group;
    bool m_usernameRevealed = false;
    bool m_passwordRevealed = false;
    bool m_totpRevealed = false;
    bool m_notesRevealed = false;

    QStackedLayout* m_pages;
    QLabel* m_emptyTitle;
    QLabel* m_emptyText;

    QLabel* m_icon;
    QLabel* m_title;
    QLabel* m_subtitle;

    QWidget* m_fields;
    VaultFieldRow* m_usernameRow;
    VaultFieldRow* m_passwordRow;
    VaultFieldRow* m_totpRow;
    VaultFieldRow* m_urlRow;
    fluent::basicinput::Button* m_usernameRevealButton;
    fluent::basicinput::Button* m_passwordRevealButton;
    fluent::basicinput::Button* m_totpRevealButton;

    QLabel* m_meta;
    QLabel* m_notesLabel;
    QLabel* m_notes;
    fluent::basicinput::Button* m_notesRevealButton;

    QTimer* m_totpTimer;
};

#endif // KEEPASSXC_ENTRYDETAILCARD_H
