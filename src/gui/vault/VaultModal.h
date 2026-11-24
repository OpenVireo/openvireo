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

#ifndef OPENVIREO_VAULTMODAL_H
#define OPENVIREO_VAULTMODAL_H

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QWidget>

#include <functional>

class QDialog;
class QHBoxLayout;
class QLabel;
class QPushButton;

namespace fluent::navigation
{
    class Stepper;
}

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

    // Makes a dialog the content of a card over its parent's window, sized to the content, and closes the card with
    // the dialog. Returns nullptr when the dialog should open as a window instead: the parent's window is hidden,
    // minimized or itself a dialog, where a card would sit inside that dialog. The caller adds any footer and opens it.
    static VaultModal* forDialog(QDialog* dialog, const QString& namePrefix, int maximumCardWidth);

    void open();
    void close();
    bool isOpen() const;

    QWidget* card() const;
    QWidget* content() const;

    void setTitle(const QString& title);

    // Adds a button to the right of the footer, named namePrefix + name
    QPushButton* addFooterButton(const QString& text, bool primary, const QString& name);
    // Adds a widget to the footer after its buttons, such as a bar of buttons standing in for a dialog's own
    void addFooterWidget(QWidget* widget, int stretch = 0);
    // A modal whose content has no buttons to put there shows no footer
    void setFooterHidden(bool hidden);
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
    QWidget* m_footerDivider;
    QWidget* m_footerWidget;
    QHBoxLayout* m_footer;
    bool m_fitToContent = false;
    int m_maximumCardWidth = 1000;
};

/**
 * Numbered steps down the side of a modal that stands in for a wizard, as a Fluent Stepper: finished steps carry a
 * check and the current one is highlighted. A finished step can be clicked to go back to it.
 */
class VaultStepList : public QWidget
{
public:
    VaultStepList(QStringList titles, QWidget* parent, Qt::Orientation orientation = Qt::Vertical);

    void setCurrent(int current);
    fluent::navigation::Stepper* stepper() const;

    std::function<void(int)> onStepClicked;

private:
    QStringList m_titles;
    fluent::navigation::Stepper* m_stepper;
    int m_current = 0;
};

#endif // OPENVIREO_VAULTMODAL_H
