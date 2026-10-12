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

#ifndef KEEPASSXC_VAULTSETTINGSPAGE_H
#define KEEPASSXC_VAULTSETTINGSPAGE_H

#include <QPointer>
#include <QWidget>

class QAbstractButton;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QVBoxLayout;
class VaultCheckBox;
class VaultSettingsCard;
class VaultSettingsRow;
class VaultSettingsSection;

// Caption of a setting: a label of the form, which keeps its buddy and mnemonic, or a title
struct VaultSettingsCaption
{
    VaultSettingsCaption(QLabel* label)
        : label(label)
    {
    }
    VaultSettingsCaption(const QString& title)
        : title(title)
    {
    }

    // Description label kept by the caller, for a description that changes
    VaultSettingsCaption(QLabel* label, QLabel* description)
        : label(label)
        , description(description)
    {
    }

    QLabel* label = nullptr;
    QString title;
    QLabel* description = nullptr;
};

namespace fluent::basicinput
{
    class Button;
    class ToggleSwitch;
} // namespace fluent::basicinput

/**
 * Lays a settings page out as Fluent settings cards, after Windows 11 Settings:
 * a page title, section headings, and a card per setting with its label on the
 * left and its control on the right.
 *
 * The page keeps its form and its logic. The builder moves the form's own
 * widgets into the cards, so the page's code and its object names keep
 * working, and the rest of the form is kept out of sight. A card or a row
 * hides while the control it shows is hidden.
 *
 *     VaultSettingsPage page(m_generalWidget, tr("General"));
 *     auto startup = page.addSection(tr("Startup"));
 *     startup->addToggle(m_ui->singleInstanceCheckBox);
 *     startup->addSetting(m_ui->languageLabel, m_ui->languageComboBox);
 *     page.finish();
 */
class VaultSettingsPage
{
    Q_DISABLE_COPY(VaultSettingsPage)

public:
    // No title heading for an empty title, as on a page whose tab or step already names it
    VaultSettingsPage(QWidget* page, const QString& title);

    // Widget between the title and the sections, such as a status message, shown while the widget is
    void addWidget(QWidget* widget);

    // A heading followed by cards; no heading for an empty title
    VaultSettingsSection* addSection(const QString& title = {});
    // Section in place of a group box of the form, titled like it and hidden and disabled with it
    VaultSettingsSection* addSection(QGroupBox* group);
    // Show the cards in place of the form. Warns about controls of the form left out of the cards.
    void finish();

private:
    QPointer<QWidget> m_page;
    QWidget* m_body;
    QVBoxLayout* m_layout;
};

class VaultSettingsSection : public QWidget
{
    Q_OBJECT

public:
    VaultSettingsSection(const QString& title, QWidget* parent);

    // Card that toggles a check box with a switch
    VaultSettingsCard* addToggle(QAbstractButton* checkBox, const QString& description = {});
    // Card with a caption on the left and one or more controls on the right
    VaultSettingsCard*
    addSetting(const VaultSettingsCaption& caption, QWidget* control, const QString& description = {});
    VaultSettingsCard*
    addSetting(const VaultSettingsCaption& caption, const QWidgetList& controls, const QString& description = {});
    // Card holding a widget across its full width, such as a table or a row of buttons
    VaultSettingsCard* addWidget(QWidget* widget);
    // Widget between the heading and the cards, outside of a card, such as a selector between sets of cards
    void addHeaderWidget(QWidget* widget, Qt::Alignment alignment = Qt::AlignLeft);

    // Hide and disable the section with a widget of the form, such as the group box it replaces
    void follow(QWidget* source);
    void updateVisibility();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    VaultSettingsCard* addCard(VaultSettingsCard* card);

    QPointer<QWidget> m_source;
    QLabel* m_heading;
    QVBoxLayout* m_headerWidgets;
    QVBoxLayout* m_cards;
};

class VaultSettingsCard : public QWidget
{
    Q_OBJECT

public:
    explicit VaultSettingsCard(QWidget* parent);

    // Header contents; each card has one of these
    void setToggle(QAbstractButton* checkBox, const QString& description);
    void setSetting(const VaultSettingsCaption& caption, const QWidgetList& controls, const QString& description);
    void setWidget(QWidget* widget);

    // Control placed in the header before the main one, such as a count next to a switch
    void addHeaderControl(QWidget* control);

    // Rows below the header that fold away behind a chevron, as in WinUI's SettingsExpander
    void addToggle(QAbstractButton* checkBox, const QString& description = {});
    void addSetting(const VaultSettingsCaption& caption, QWidget* control, const QString& description = {});
    void addSetting(const VaultSettingsCaption& caption, const QWidgetList& controls, const QString& description = {});
    void addWidget(QWidget* widget);

    // Rows that always show, without a chevron to fold them away; set before adding rows
    void setCollapsible(bool collapsible);
    // Hide the card with a widget of the form, such as the tab page it replaces; with several, while any is hidden
    void follow(QWidget* source);

    bool isExpanded() const;
    void setExpanded(bool expanded);

signals:
    void visibilityChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct Row
    {
        VaultSettingsRow* row;
        // The row shows while any of these does
        QList<QPointer<QWidget>> controls;
    };

    void fillToggleRow(VaultSettingsRow* row, QAbstractButton* checkBox, const QString& description);
    void fillSettingRow(VaultSettingsRow* row,
                        const VaultSettingsCaption& caption,
                        const QWidgetList& controls,
                        const QString& description);
    VaultSettingsRow* addRow();
    void watch(const QWidgetList& controls);
    static bool anyShown(const QList<QPointer<QWidget>>& controls);
    void updateRows();

    VaultSettingsRow* m_header;
    bool m_toggleHeader = false;
    QList<QPointer<QWidget>> m_controls;
    QWidget* m_focusRow = nullptr;
    bool m_keyboardFocus = false;
    fluent::basicinput::Button* m_chevron = nullptr;
    QWidget* m_children;
    QVBoxLayout* m_childLayout;
    QList<Row> m_rows;
    QList<QPointer<QWidget>> m_sources;
    bool m_collapsible = true;
    bool m_expanded = true;
};

#endif // KEEPASSXC_VAULTSETTINGSPAGE_H
