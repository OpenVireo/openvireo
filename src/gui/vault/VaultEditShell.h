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

#ifndef KEEPASSXC_VAULTEDITSHELL_H
#define KEEPASSXC_VAULTEDITSHELL_H

#include <QDialogButtonBox>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <components/foundation/FluentElement.h>

class CategoryListWidget;
class QHBoxLayout;
class QLabel;
class QStackedWidget;
class VaultSurface;

/**
 * Fluent buttons standing in for a QDialogButtonBox.
 *
 * The box stays in place, hidden, so the form keeps changing its standard
 * buttons and reacting to them as before; each Fluent button follows one of
 * the box's buttons and clicks it.
 */
class VaultButtonBar : public QWidget
{
    Q_OBJECT

public:
    VaultButtonBar(QDialogButtonBox* buttons, QWidget* parent = nullptr);

    // Label a standard button differently from the box, or leave it out of the bar
    void setButtonText(QDialogButtonBox::StandardButton button, const QString& text);
    void setButtonHidden(QDialogButtonBox::StandardButton button, bool hidden);
    // Share the bar's width equally between the buttons, as in a WinUI content dialog
    void setFillWidth(bool fill);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    bool isHiddenInBar(QAbstractButton* source) const;

    QPointer<QDialogButtonBox> m_buttons;
    QHash<int, QString> m_texts;
    QSet<int> m_hidden;
    QHBoxLayout* m_layout;
    bool m_fillWidth = false;
    bool m_rebuildPending = false;
};

/**
 * Presents an editor made of a category list, a page stack and a button box
 * (the entry, group and database settings editors and the reports) in the
 * Vault Index style: categories as a navigation pane on the window canvas,
 * and the headline, pages and buttons on a rounded surface beside it.
 *
 * Only the presentation changes; the form's widgets and logic are untouched.
 */
class VaultEditShell : public QObject, public fluent::FluentElement
{
    Q_OBJECT

public:
    static VaultEditShell* apply(QWidget* form,
                                 CategoryListWidget* categories,
                                 QStackedWidget* pages,
                                 QDialogButtonBox* buttons,
                                 QLabel* headline = nullptr);

    // Headline for forms that don't have their own
    void setTitle(const QString& title);
    void onThemeUpdated() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    VaultEditShell(QWidget* form, CategoryListWidget* categories);
    void clearPageBackgrounds();

    QPointer<CategoryListWidget> m_categories;
    QPointer<QStackedWidget> m_pages;
    QPointer<QLabel> m_headline;
    QLabel* m_title = nullptr;
    VaultSurface* m_surface = nullptr;
};

#endif // KEEPASSXC_VAULTEDITSHELL_H
