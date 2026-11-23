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

#include "VaultInputDialog.h"

#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultDialog.h"

#include <components/dialogs_flyouts/ContentDialog.h>
#include <components/status_info/ProgressRing.h>
#include <components/textfields/Label.h>

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    // As wide as a message box's content dialog, with room for the field
    constexpr int InputDialogWidth = 440;

    bool canShowOver(QWidget* window)
    {
        return window && window->isVisible() && !window->isMinimized();
    }

    fluent::textfields::Label* bodyLabel(const QString& text, QWidget* parent)
    {
        using fluent::textfields::Label;
        auto* label = new Label(parent);
        label->setObjectName(QStringLiteral("vaultInputLabel"));
        label->setFluentTypography(Typography::FontRole::Body);
        label->setTextColorRole(Label::TextColorRole::Primary);
        label->setWordWrap(true);
        label->setText(text);
        return label;
    }

    // The content dialog's primary button: Enter presses it, and it is the accent one
    void makePrimaryDefault(fluent::dialogs_flyouts::ContentDialog& dialog)
    {
        for (auto* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == dialog.primaryButtonText()) {
                button->setDefault(true);
            }
        }
    }
} // namespace

VaultInputDialog::VaultInputDialog(QWidget* parent)
    : QObject(parent)
    , m_parent(parent)
{
}

VaultInputDialog::~VaultInputDialog() = default;

void VaultInputDialog::setWindowTitle(const QString& title)
{
    m_title = title;
}

void VaultInputDialog::setLabelText(const QString& text)
{
    m_label = text;
}

void VaultInputDialog::setTextValue(const QString& text)
{
    m_text = text;
}

void VaultInputDialog::setOkButtonText(const QString& text)
{
    m_okText = text;
}

void VaultInputDialog::setComboBoxItems(const QStringList& items)
{
    m_items = items;
    m_useItems = true;
}

void VaultInputDialog::setStaysOnTop(bool onTop)
{
    m_staysOnTop = onTop;
}

QString VaultInputDialog::textValue() const
{
    return m_text;
}

void VaultInputDialog::reject()
{
    if (m_dialog) {
        m_dialog->reject();
    }
}

int VaultInputDialog::exec()
{
    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    if (!m_label.isEmpty()) {
        layout->addWidget(bodyLabel(m_label, content));
    }

    QLineEdit* line = nullptr;
    VaultComboBox* combo = nullptr;
    QWidget* field = nullptr;
    if (m_useItems) {
        combo = new VaultComboBox(content);
        combo->setEditable(true);
        combo->addItems(m_items);
        combo->setCurrentText(m_text);
        field = combo;
    } else {
        line = new VaultLineEdit(content);
        line->setText(m_text);
        line->selectAll();
        field = line;
    }
    field->setObjectName(QStringLiteral("vaultInputField"));
    layout->addWidget(field);
    const auto okText = m_okText.isEmpty() ? tr("OK") : m_okText;
    const auto readValue = [line, combo] { return line ? line->text() : combo->currentText(); };

    auto* window = m_parent ? m_parent->window() : nullptr;
    int result = QDialog::Rejected;
    if (canShowOver(window) && !m_staysOnTop) {
        using fluent::dialogs_flyouts::ContentDialog;
        ContentDialog dialog(window);
        m_dialog = &dialog;
        dialog.setObjectName(QStringLiteral("vaultInputDialog"));
        dialog.setThemeSource(window);
        dialog.setTitle(m_title);
        dialog.setPrimaryButtonText(VaultButton::withoutMnemonic(okText));
        dialog.setCloseButtonText(tr("Cancel"));
        dialog.setDefaultButton(ContentDialog::Primary);
        dialog.setContent(content);
        dialog.resize(InputDialogWidth + 2 * dialog.shadowSize(), dialog.minimumHeight());
        makePrimaryDefault(dialog);
        // The dialog takes the focus on opening; the field takes it from there
        QTimer::singleShot(0, field, [field] { field->setFocus(Qt::PopupFocusReason); });
        result = dialog.exec() == ContentDialog::ResultPrimary ? QDialog::Accepted : QDialog::Rejected;
        m_text = readValue();
    } else {
        // A Fluent window of its own, laid out like the content dialog
        VaultDialog dialog(m_parent);
        m_dialog = &dialog;
        dialog.setObjectName(QStringLiteral("vaultInputDialog"));
        dialog.setPresentation(VaultDialog::OwnWindow);
        dialog.setWindowTitle(m_title);
        if (m_staysOnTop) {
            dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowStaysOnTopHint);
        }
        auto* dialogLayout = new QVBoxLayout(&dialog);
        dialogLayout->addWidget(content);
        auto* buttons = new VaultDialogButtonBox(&dialog);
        buttons->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(okText);
        buttons->button(QDialogButtonBox::Ok)->setDefault(true);
        dialogLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.setCommands(buttons);
        dialog.setInitialFocus(field);
        dialog.setMaximumDialogWidth(InputDialogWidth + 48);
        result = dialog.exec();
        m_text = readValue();
    }
    m_dialog.clear();
    return result;
}

QString VaultInputDialog::getText(QWidget* parent,
                                  const QString& title,
                                  const QString& label,
                                  const QString& text,
                                  bool* ok)
{
    VaultInputDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(text);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (ok) {
        *ok = accepted;
    }
    return accepted ? dialog.textValue() : QString();
}

VaultProgressDialog::VaultProgressDialog(QWidget* parent,
                                         const QString& title,
                                         const QString& label,
                                         const QString& cancelText,
                                         int maximum)
    : QObject(parent)
    , m_parent(parent)
    , m_title(title)
    , m_label(label)
    , m_cancelText(cancelText)
    , m_maximum(maximum)
{
    m_timer.start();
}

VaultProgressDialog::~VaultProgressDialog()
{
    reset();
}

int VaultProgressDialog::value() const
{
    return m_value;
}

void VaultProgressDialog::setMinimumDuration(int milliseconds)
{
    m_minimumDuration = milliseconds;
}

bool VaultProgressDialog::wasCanceled() const
{
    return m_canceled;
}

void VaultProgressDialog::setValue(int value)
{
    m_value = value;
    if (!m_dialog && !m_canceled && value < m_maximum && m_timer.elapsed() >= m_minimumDuration) {
        show();
    }
    if (m_ring) {
        m_ring->setValue(value);
    }
    // Keeps the dialog drawn and its button pressable while the task runs
    if (m_dialog) {
        QCoreApplication::processEvents();
    }
}

void VaultProgressDialog::show()
{
    auto* window = m_parent ? m_parent->window() : nullptr;
    if (!canShowOver(window)) {
        return;
    }
    using fluent::dialogs_flyouts::ContentDialog;
    auto* dialog = new ContentDialog(window);
    dialog->setObjectName(QStringLiteral("vaultProgressDialog"));
    dialog->setThemeSource(window);
    dialog->setTitle(m_title);
    dialog->setCloseButtonText(m_cancelText);
    dialog->setClosePolicy(fluent::dialogs_flyouts::Dialog::NoAutoClose);

    auto* content = new QWidget();
    auto* layout = new QHBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    auto* ring = new fluent::status_info::ProgressRing(content);
    ring->setObjectName(QStringLiteral("vaultProgressRing"));
    ring->setIsIndeterminate(m_maximum <= 0);
    ring->setMinimum(0);
    ring->setMaximum(qMax(m_maximum, 1));
    ring->setValue(m_value);
    ring->setIsActive(true);
    layout->addWidget(ring, 0, Qt::AlignVCenter);
    layout->addWidget(bodyLabel(m_label, content), 1);
    dialog->setContent(content);
    dialog->resize(InputDialogWidth + 2 * dialog->shadowSize(), dialog->minimumHeight());

    connect(dialog, &ContentDialog::closeButtonClicked, this, [this] { m_canceled = true; });
    connect(dialog, &QDialog::rejected, this, [this] { m_canceled = true; });
    m_dialog = dialog;
    m_ring = ring;
    dialog->open();
}

void VaultProgressDialog::reset()
{
    if (!m_dialog) {
        return;
    }
    // Done, not canceled
    disconnect(m_dialog, nullptr, this, nullptr);
    m_dialog->hide();
    m_dialog->deleteLater();
    m_dialog.clear();
    m_ring.clear();
}
