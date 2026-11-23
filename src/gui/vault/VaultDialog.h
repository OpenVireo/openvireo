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

#ifndef OPENVIREO_VAULTDIALOG_H
#define OPENVIREO_VAULTDIALOG_H

#include <QDialog>
#include <QList>
#include <QPointer>

class QAbstractButton;
class QDialogButtonBox;
class QLabel;
class QLayout;
class VaultModal;

/**
 * A dialog shown the Fluent way, wherever it opens.
 *
 * Over the main window it is a VaultModal card: the card gives the title and the close button, the dialog is
 * its content, and the dialog's buttons show in the card's footer. Without a window to show over, over another
 * dialog, or when it always stands alone (Auto-Type, browser and Secret Service prompts), it is a Fluent window
 * of its own laid out like a WinUI content dialog: the title, the content on the layer fill, and the buttons on
 * the canvas below a divider, sharing its width.
 *
 * Subclasses stay ordinary QDialogs: show(), open() and exec() work as before, and so do done(), accept(),
 * reject() and the finished signal, which closes the card. The dialog's button box stays in the dialog, hidden,
 * with Fluent buttons standing in for its buttons, so the dialog's code and tests keep using it.
 */
class VaultDialog : public QDialog
{
    Q_OBJECT

public:
    enum Presentation
    {
        // A card over the main window when there is one to show over, a window of its own otherwise
        CardOverWindow,
        // Always a window of its own
        OwnWindow,
    };

    explicit VaultDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = {});
    ~VaultDialog() override;

    void setPresentation(Presentation presentation);
    Presentation presentation() const;

    // Object names of the card's parts start with this, such as "vaultCloneCard" for "vaultClone"
    void setNamePrefix(const QString& prefix);
    // Widest the card or the window gets
    void setMaximumDialogWidth(int width);
    // The card fills the window's height instead of fitting its content, for content that scrolls
    void setFillHeight(bool fill);

    // The buttons shown in the card's footer or the window's command bar: the dialog's button box, which is
    // found by itself if there is one, or loose buttons of the form, which move there
    void setCommands(QDialogButtonBox* buttons);
    void setCommands(const QList<QAbstractButton*>& buttons);
    QDialogButtonBox* commandBox() const;

    // Widget focused on opening
    void setInitialFocus(QWidget* widget);

    // The card while the dialog shows over the main window
    VaultModal* modal() const;
    bool isCard() const;

    void setVisible(bool visible) override;

    // Whether a dialog over parent would show as a card over its window
    static bool canShowCardOver(const QWidget* parent);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

    // Called each time before the dialog shows, as a card or a window
    virtual void prepareToShow();

private:
    void presentAsCard();
    void dismissCard();
    void applyWindowChrome();
    void fitWindow();
    void clickPrimary();
    QDialogButtonBox* findCommandBox() const;
    void moveLooseCommands(QWidget* host, bool fillWidth);
    void restoreLooseCommands();

    Presentation m_presentation = CardOverWindow;
    QString m_namePrefix;
    int m_maximumWidth = 560;
    bool m_fillHeight = false;
    QPointer<QDialogButtonBox> m_commandBox;
    bool m_commandBoxSet = false;

    struct LooseCommand
    {
        QPointer<QAbstractButton> button;
        QPointer<QLayout> layout;
        int index;
    };
    QList<LooseCommand> m_looseCommands;
    QPointer<QWidget> m_initialFocus;

    // While shown as a card
    QPointer<VaultModal> m_modal;
    QPointer<QWidget> m_cardParent;
    Qt::WindowFlags m_cardWindowFlags;
    bool m_showingInCard = false;

    // The window's own title, body and command bar, made the first time it shows as a window
    QLabel* m_windowTitle = nullptr;
    QWidget* m_windowBody = nullptr;
    QWidget* m_windowCommands = nullptr;
};

#endif // OPENVIREO_VAULTDIALOG_H
