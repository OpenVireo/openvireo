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

#ifndef OPENVIREO_VAULTCONTROLS_H
#define OPENVIREO_VAULTCONTROLS_H

#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QResizeEvent>

#include <components/basicinput/Button.h>
#include <components/basicinput/CheckBox.h>
#include <components/basicinput/ComboBox.h>
#include <components/basicinput/RadioButton.h>
#include <components/basicinput/SplitButton.h>
#include <components/foundation/FluentElement.h>
#include <components/layout/Card.h>
#include <components/menus_toolbars/Menu.h>
#include <components/status_info/ProgressBar.h>
#include <components/textfields/LineEdit.h>
#include <components/textfields/NumberBox.h>

/**
 * Fluent-Qt controls as drop-in replacements for the stock ones in .ui forms.
 *
 * The forms set icons, text and default buttons the Qt way, so these adapt
 * the Fluent controls to that instead of needing Fluent-specific setup.
 */

// Push button that lays out a QIcon set by the form and gives the default button the accent style
class VaultButton : public fluent::basicinput::Button
{
    Q_OBJECT

public:
    // Text as displayed for a Qt mnemonic label: "&Open" reads "Open" and "A && B" reads "A & B"
    static QString withoutMnemonic(const QString& text);

    explicit VaultButton(QWidget* parent = nullptr);
    explicit VaultButton(const QString& text, QWidget* parent = nullptr);

    // Filled with the critical color, for an action that can't be undone such as deleting the account
    void setDestructive(bool destructive);
    bool isDestructive() const;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void syncAppearance();
    void paintDestructive();

    bool m_destructive = false;

    // Text last shown without its mnemonic, so a literal "&" left in it isn't stripped again
    QString m_plainText;
};

// Split button for forms, in place of a QToolButton with a menu: the main part clicks, the chevron opens the menu
class VaultSplitButton : public fluent::basicinput::SplitButton
{
    Q_OBJECT

public:
    explicit VaultSplitButton(QWidget* parent = nullptr)
        : fluent::basicinput::SplitButton(QString(), parent)
    {
    }
};

// Progress bar for forms, in place of QProgressBar: a Fluent progress bar that fills the width its layout gives it
class VaultProgressBar : public fluent::status_info::ProgressBar
{
    Q_OBJECT

public:
    explicit VaultProgressBar(QWidget* parent = nullptr)
        : fluent::status_info::ProgressBar(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        return {120, fluent::status_info::ProgressBar::sizeHint().height()};
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        // The Fluent bar draws no wider than its bar width
        setBarWidth(qMax(1, width()));
        fluent::status_info::ProgressBar::resizeEvent(event);
    }
};

// Check box with WinUI's stronger outline while unchecked, so it stays visible on light surfaces
class VaultCheckBox : public fluent::basicinput::CheckBox
{
    Q_OBJECT

public:
    explicit VaultCheckBox(QWidget* parent = nullptr);
    explicit VaultCheckBox(const QString& text, QWidget* parent = nullptr);

    // Draw only the text, wrapped, with a description below it and no box, for a settings
    // card that shows a toggle switch for the box instead
    void setSwitchStyle(bool switchStyle);
    bool hasSwitchStyle() const;
    void setDescription(const QString& description);
    QString description() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

protected:
    // The whole control toggles the box, as in WinUI, not only the box and its label
    bool hitButton(const QPoint& pos) const override;
    void paintEvent(QPaintEvent* event) override;

private:
    int switchTextHeight(int width) const;

    bool m_switchStyle = false;
    QString m_description;
};

// Radio button for forms, so they can use the Fluent one in place of the stock one
class VaultRadioButton : public fluent::basicinput::RadioButton
{
    Q_OBJECT

public:
    explicit VaultRadioButton(QWidget* parent = nullptr);
    explicit VaultRadioButton(const QString& text, QWidget* parent = nullptr);
};

// Combo box that, like QComboBox, handles keys sent to it while it doesn't have focus
class VaultComboBox : public fluent::basicinput::ComboBox
{
    Q_OBJECT

public:
    explicit VaultComboBox(QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
};

// Line edit without Fluent's clear button unless the form enables it
class VaultLineEdit : public fluent::textfields::LineEdit
{
    Q_OBJECT

public:
    explicit VaultLineEdit(QWidget* parent = nullptr);

    // Fields inside a widget with this property set get the roomier padding of the unlock card's password
    static constexpr const char* LargePaddingProperty = "vaultLineEditLargePadding";
    static const QMargins LargePadding;

protected:
    void showEvent(QShowEvent* event) override;
};

// Paints the Fluent line edit's fill, border and focus underline behind a stock field, and gives it Fluent text
// colors, so fields Fluent-Qt has no drop-in replacement for still match the Fluent ones beside them. The field
// must not paint its own frame or background.
class VaultFieldFrame : public QObject, public fluent::FluentElement
{
    Q_OBJECT

public:
    static void install(QWidget* field);

    void onThemeUpdated() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit VaultFieldFrame(QWidget* field);
    void applyPalette();
    void paint();

    QWidget* m_field;
};

// Multi-line text field with the Fluent frame. It stays a QPlainTextEdit, rather than Fluent-Qt's TextEdit, so
// forms keep its API, the font they set and the height their layout gives it.
class VaultPlainTextEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit VaultPlainTextEdit(QWidget* parent = nullptr);

    QMargins contentMargins() const;
    void setContentMargins(const QMargins& margins);

private:
    QMargins m_contentMargins;
};

// Date and time field with the Fluent frame and a Fluent chevron for its calendar. Fluent-Qt's date picker has no
// time, so this stays a QDateTimeEdit.
class VaultDateTimeEdit : public QDateTimeEdit
{
    Q_OBJECT

public:
    explicit VaultDateTimeEdit(QWidget* parent = nullptr);

    QMargins contentMargins() const;
    void setContentMargins(const QMargins& margins);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void applyStyleSheet();

    QMargins m_contentMargins;
};

// Number field for forms, in place of QSpinBox: a Fluent number box with the integer API of QSpinBox
// (value, range, single step, suffix, valueChanged(int)), so forms, their code and tests keep using it as before.
// The suffix shows after the number in the secondary text color.
class VaultSpinBox : public fluent::textfields::NumberBox
{
    Q_OBJECT
    Q_PROPERTY(int value READ value WRITE setValue USER true)
    Q_PROPERTY(int minimum READ minimum WRITE setMinimum)
    Q_PROPERTY(int maximum READ maximum WRITE setMaximum)
    Q_PROPERTY(int singleStep READ singleStep WRITE setSingleStep)
    Q_PROPERTY(QString suffix READ suffix WRITE setSuffix)
    Q_PROPERTY(bool keyboardTracking READ keyboardTracking WRITE setKeyboardTracking)

public:
    explicit VaultSpinBox(QWidget* parent = nullptr);

    int value() const;
    int minimum() const;
    void setMinimum(int minimum);
    int maximum() const;
    void setMaximum(int maximum);
    void setRange(int minimum, int maximum);
    int singleStep() const;
    void setSingleStep(int step);
    QString suffix() const;
    void setSuffix(const QString& suffix);
    // As with QSpinBox, the value follows the text while typing unless this is turned off
    bool keyboardTracking() const;
    void setKeyboardTracking(bool tracking);
    // The number as typed, without the suffix
    QString cleanText() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    void setValue(int value);

signals:
    void valueChanged(int value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void syncValue();
    void trackText(const QString& text);
    void restoreValidValue();
    void updateSuffixMargin();
    int suffixWidth() const;

    int m_value = 0;
    QString m_suffix;
    bool m_keyboardTracking = true;
};

// Card for forms, in place of QGroupBox: a Fluent card holding the group's controls, headed by its title
class VaultCard : public fluent::layout::Card
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle)

public:
    explicit VaultCard(QWidget* parent = nullptr);
    explicit VaultCard(const QString& title, QWidget* parent = nullptr);

    QString title() const;
    void setTitle(const QString& title);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void updateTitleMargin();
    QRect titleRect() const;

    QString m_title;
};

// Dialog button box for forms that shows Fluent buttons. The box keeps the whole QDialogButtonBox API for the
// form's code and tests, but is presented through a VaultButtonBar: a form that doesn't put it in a bar itself,
// or in a modal's footer, gets one in its place the first time the box shows.
class VaultDialogButtonBox : public QDialogButtonBox
{
    Q_OBJECT

public:
    explicit VaultDialogButtonBox(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void present();

    bool m_presentPending = false;
};

// Fluent menu for context menus, button menus and menu bar menus
class VaultMenu : public fluent::menus_toolbars::FluentMenu
{
    Q_OBJECT

public:
    explicit VaultMenu(QWidget* parent = nullptr);
    explicit VaultMenu(const QString& title, QWidget* parent = nullptr);

    // QMenu::addMenu() with a title creates a stock submenu, so create a Fluent one instead
    using QMenu::addMenu;
    VaultMenu* addMenu(const QString& title);
    VaultMenu* addMenu(const QIcon& icon, const QString& title);

    // Show Fluent editing menus instead of the stock ones for every text field in the application
    static void installTextEditingMenus();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QPointer<QAction> m_pressedAction;
};

#endif // OPENVIREO_VAULTCONTROLS_H
