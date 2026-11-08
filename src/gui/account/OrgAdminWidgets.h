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

#ifndef KEEPASSXC_ORGADMINWIDGETS_H
#define KEEPASSXC_ORGADMINWIDGETS_H

#include "gui/vault/VaultModal.h"

#include <design/Typography.h>

#include <QPointer>
#include <QStyledItemDelegate>

class QDialog;
class QLabel;
class QPushButton;
class QVBoxLayout;
class ServiceAccountsDialog;

/**
 * Pieces of the Organization window (ServiceAccountsDialog): list rows, the sidebar, prompts and the modal
 * card it sits in within the Vault Index layout.
 */
namespace OrgAdmin
{
    // Item data the row delegate draws, next to Qt::DisplayRole as the main text
    enum Role
    {
        SecondaryRole = Qt::UserRole + 10,
        BadgeRole,
        BadgeKindRole,
        // Draw the initials of the main text in a circle before it
        AvatarRole,
        // The number shown after a sidebar section's name, such as members waiting to be confirmed
        CountRole,
        // A Typography::Icons glyph before a sidebar section's name
        GlyphRole,
    };

    enum class BadgeKind
    {
        Neutral,
        Accent,
        Caution,
        Success,
        Critical,
    };

    QLabel* label(QWidget* parent, Typography::FontRole role, bool secondary = false);
    // Page heading with a description below it
    QWidget* pageHeader(const QString& title, const QString& description, QWidget* action, QWidget* parent);
    // Fluent look for a list drawn by RowDelegate, and for a table with a header
    QString listStyleSheet();
    QString tableStyleSheet();

    /**
     * One row of a list as a Fluent list item: an optional avatar, the main text with a line of secondary
     * text below it, and a badge at the end. Hover and selection are drawn as subtle fills, with the accent
     * pill WinUI puts at the start of the selected row.
     */
    class RowDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };

    // Sidebar rows: a glyph, the section's name and a count badge, as in Settings' sidebar
    class NavigationDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };

    /**
     * A small form with a title, an accept button and Cancel. It is a Fluent content dialog over the window
     * in the Vault Index layout, and a plain dialog otherwise, as MessageBox does.
     */
    class Prompt
    {
    public:
        Prompt(QWidget* parent, const QString& title, const QString& acceptText, const QString& objectName);
        ~Prompt();
        Q_DISABLE_COPY(Prompt)

        QWidget* content() const;
        QVBoxLayout* layout() const;
        // A paragraph of body text
        QLabel* addText(const QString& text);
        // The button that accepts, to enable it only once the form is complete
        QPushButton* acceptButton() const;
        // True when the form was accepted
        bool exec();

        static bool canShowFluent(QWidget* parent);

    private:
        QDialog* m_dialog;
        QWidget* m_content;
        QVBoxLayout* m_layout;
        QPushButton* m_accept = nullptr;
        bool m_fluent;
    };
} // namespace OrgAdmin

/**
 * The Organization window as a card over the main window in the Vault Index layout, like Settings: the window's
 * own sidebar and pages in the middle, the title and close button above them and Done below.
 */
class OrgAdminModal : public VaultModal
{
    Q_OBJECT

public:
    OrgAdminModal(QWidget* window, ServiceAccountsDialog* dialog);

    ServiceAccountsDialog* dialog() const;

protected:
    // Return belongs to the focused button or field, not to the card
    void accept() override;
    void reject() override;

private:
    QPointer<ServiceAccountsDialog> m_dialog;
};

#endif // KEEPASSXC_ORGADMINWIDGETS_H
