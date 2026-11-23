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

#ifndef OPENVIREO_VAULTINPUTDIALOG_H
#define OPENVIREO_VAULTINPUTDIALOG_H

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QDialog;
class QWidget;

namespace fluent::status_info
{
    class ProgressRing;
}

/**
 * Asks for a line of text, in place of QInputDialog: a Fluent content dialog over the parent's window, with the
 * label, a text field and OK and Cancel. Without a window to show over, such as for a browser's request while
 * OpenVireo is hidden, it is a Fluent window of its own laid out the same way.
 *
 *     VaultInputDialog dialog(this);
 *     dialog.setWindowTitle(tr("Save Search"));
 *     dialog.setLabelText(tr("Name of the search:"));
 *     if (dialog.exec() == QDialog::Accepted) { use(dialog.textValue()); }
 */
class VaultInputDialog : public QObject
{
    Q_OBJECT

public:
    explicit VaultInputDialog(QWidget* parent);
    ~VaultInputDialog() override;

    void setWindowTitle(const QString& title);
    void setLabelText(const QString& text);
    void setTextValue(const QString& text);
    void setOkButtonText(const QString& text);
    // Choices to pick from or type over, in an editable combo box instead of a text field
    void setComboBoxItems(const QStringList& items);
    // Stays above other applications' windows, for requests from them
    void setStaysOnTop(bool onTop);

    QString textValue() const;

    // QDialog::Accepted or QDialog::Rejected
    int exec();

    static QString getText(QWidget* parent,
                           const QString& title,
                           const QString& label,
                           const QString& text = {},
                           bool* ok = nullptr);

public slots:
    void reject();

private:
    QPointer<QWidget> m_parent;
    QString m_title;
    QString m_label;
    QString m_text;
    QString m_okText;
    QStringList m_items;
    bool m_useItems = false;
    bool m_staysOnTop = false;
    QPointer<QDialog> m_dialog;
};

/**
 * Shows the progress of a long task, in place of QProgressDialog: a Fluent content dialog over the parent's window
 * with a progress ring and a button to stop. Like QProgressDialog it shows only once the task takes a moment, and
 * setValue() keeps the application responsive so the button can be pressed.
 */
class VaultProgressDialog : public QObject
{
    Q_OBJECT

public:
    VaultProgressDialog(QWidget* parent, const QString& title, const QString& label, const QString& cancelText, int maximum);
    ~VaultProgressDialog() override;

    int value() const;
    void setValue(int value);
    bool wasCanceled() const;
    // Closes the dialog, as when the task is done
    void reset();

    // Milliseconds the task runs before the dialog shows
    void setMinimumDuration(int milliseconds);

private:
    void show();

    QPointer<QWidget> m_parent;
    QString m_title;
    QString m_label;
    QString m_cancelText;
    int m_maximum;
    int m_value = 0;
    int m_minimumDuration = 500;
    bool m_canceled = false;
    QElapsedTimer m_timer;
    QPointer<QDialog> m_dialog;
    QPointer<fluent::status_info::ProgressRing> m_ring;
};

#endif // OPENVIREO_VAULTINPUTDIALOG_H
