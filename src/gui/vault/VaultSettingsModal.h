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

#ifndef KEEPASSXC_VAULTSETTINGSMODAL_H
#define KEEPASSXC_VAULTSETTINGSMODAL_H

#include <QPointer>
#include <QWidget>

class EditWidget;
class QLabel;
class QListView;
class QPushButton;

/**
 * Presents a settings widget as a modal card over the main window, styled
 * after Slack's preferences: a dimmed backdrop, a rounded card with a title
 * and close button, a sidebar of categories and Cancel / Save in the footer.
 *
 * The settings widget keeps its own pages and logic; the modal only hides its
 * category list and buttons and drives them from its own controls.
 */
class VaultSettingsModal : public QWidget
{
    Q_OBJECT

public:
    VaultSettingsModal(QWidget* window, EditWidget* settings);

    void open();
    void close();
    bool isOpen() const;

    QWidget* card() const;
    QListView* navigation() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void changeEvent(QEvent* event) override;

private:
    void accept();
    void reject();
    void applyColors();
    void syncNavigation();
    void layoutCard();

    QPointer<QWidget> m_window;
    QPointer<EditWidget> m_settings;
    QPointer<QWidget> m_previousFocus;
    QWidget* m_card;
    QLabel* m_title;
    QListView* m_navigation;
    QPushButton* m_cancelButton;
    QPushButton* m_saveButton;
    QList<QWidget*> m_dividers;
};

#endif // KEEPASSXC_VAULTSETTINGSMODAL_H
