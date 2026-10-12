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

#ifndef KEEPASSXC_VAULTMODAL_H
#define KEEPASSXC_VAULTMODAL_H

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPushButton;

/**
 * Presents a widget as a modal card over the main window, styled after
 * Slack's preferences: a dimmed backdrop, a rounded card with a title and
 * close button, the content in the middle and buttons in the footer.
 *
 * Object names of the parts start with the given prefix, such as
 * "vaultSettingsCard" and "vaultSettingsClose" for the prefix "vaultSettings".
 */
class VaultModal : public QWidget
{
    Q_OBJECT

public:
    // Colors of Slack's preferences modal
    struct Colors
    {
        QColor backdrop;
        QColor card;
        QColor border;
        QColor text;
        QColor textSecondary;
        QColor hover;
        QColor selected;
        QColor onSelected;
    };
    static Colors colors();
    static QFont pixelFont(const QFont& base, int pixelSize, QFont::Weight weight = QFont::Normal);

    VaultModal(QWidget* window, QWidget* content, const QString& title, const QString& namePrefix);

    void open();
    void close();
    bool isOpen() const;

    QWidget* card() const;
    QWidget* content() const;

    void setTitle(const QString& title);

    // Adds a button to the right of the footer, named namePrefix + name
    QPushButton* addFooterButton(const QString& text, bool primary, const QString& name);
    // Widget focused when the modal opens, the content by default
    void setInitialFocus(QWidget* widget);
    // Shrinks the card to the height of its content instead of filling the window
    void setFitToContent(bool fit);
    void setMaximumCardWidth(int width);

signals:
    // Return or Enter, unless the focused widget handled it
    void accepted();
    // Escape or the close button
    void rejected();

protected:
    virtual void accept();
    virtual void reject();
    // Called on opening, before the modal is shown
    virtual void prepare();
    virtual void applyColors();

    QHBoxLayout* bodyLayout() const;

    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void changeEvent(QEvent* event) override;

private:
    void layoutCard();

    QPointer<QWidget> m_window;
    QPointer<QWidget> m_content;
    QPointer<QWidget> m_previousFocus;
    QPointer<QWidget> m_initialFocus;
    QString m_namePrefix;
    QWidget* m_card;
    QLabel* m_title;
    QPushButton* m_closeButton;
    QHBoxLayout* m_body;
    QHBoxLayout* m_footer;
    bool m_fitToContent = false;
    int m_maximumCardWidth = 1000;
};

#endif // KEEPASSXC_VAULTMODAL_H
