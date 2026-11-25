/*
 *  Copyright (C) 2024 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ShortcutSettingsPage.h"

#include "core/Config.h"
#include "gui/ActionCollection.h"
#include "gui/Icons.h"
#include "gui/MessageBox.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultDialog.h"
#include "gui/vault/VaultWidgets.h"
#include "gui/widgets/ShortcutWidget.h"

#include <functional>

#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

// Settings › Shortcuts › an action: a card with the keys pressed in a large field, a note while another action uses
// them already, and the action's default with Restore default on the left
class KeySequenceDialog final : public VaultDialog
{
public:
    using ConflictName = std::function<QString(const QKeySequence&)>;

    KeySequenceDialog(const QString& actionName,
                      const QKeySequence& current,
                      const QKeySequence& defaultSequence,
                      ConflictName conflictName,
                      QWidget* parent = nullptr)
        : VaultDialog(parent)
        , m_keySeqEdit(new ShortcutWidget(this))
        , m_btnBox(new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel
                                            | QDialogButtonBox::RestoreDefaults,
                                        this))
        , m_intro(new QLabel(QObject::tr("Press the keys you want to use."), this))
        , m_conflict(new VaultPanel(VaultPanel::Caution, this))
        , m_conflictText(new QLabel(m_conflict))
        , m_defaultCaption(new QLabel(QObject::tr("Default"), this))
        , m_defaultKeys(new VaultPill(defaultSequence.isEmpty() ? QObject::tr("None")
                                                                : defaultSequence.toString(QKeySequence::NativeText),
                                      this))
        , m_conflictName(std::move(conflictName))
    {
        setObjectName("keySequenceDialog");
        setWindowTitle(actionName);
        setNamePrefix(QStringLiteral("vaultShortcut"));
        setMaximumDialogWidth(460);

        auto* l = new QVBoxLayout(this);
        l->setSpacing(14);
        connect(m_btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(m_btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_btnBox, &QDialogButtonBox::clicked, this, &KeySequenceDialog::restoreDefault);
        m_btnBox->button(QDialogButtonBox::Save)->setDefault(true);
        m_btnBox->button(QDialogButtonBox::RestoreDefaults)->setText(QObject::tr("Restore default"));

        m_intro->setWordWrap(true);
        l->addWidget(m_intro);

        // The keys, large and centred in a Fluent field
        m_keySeqEdit->setObjectName("keySequenceEdit");
        m_keySeqEdit->setAccessibleName(QObject::tr("Shortcut for %1").arg(actionName));
        m_keySeqEdit->setAlignment(Qt::AlignCenter);
        m_keySeqEdit->setFixedHeight(72);
        m_keySeqEdit->setPlaceholderText(QObject::tr("Hold a modifier and press a key"));
        // Starts on the keys the action has, so Save without pressing any keeps them
        if (!current.isEmpty()) {
            m_keySeqEdit->setShortcut(current[0].key(), current[0].keyboardModifiers());
        }
        l->addWidget(m_keySeqEdit);

        // Another action uses these keys
        m_conflict->setObjectName("keySequenceConflict");
        auto* conflictLayout = new QHBoxLayout(m_conflict);
        conflictLayout->setContentsMargins(14, 10, 14, 10);
        conflictLayout->setSpacing(10);
        auto* warning = new QLabel(Typography::Icons::Warning, m_conflict);
        warning->setObjectName("keySequenceConflictIcon");
        m_conflictText->setWordWrap(true);
        m_conflictText->setTextFormat(Qt::PlainText);
        conflictLayout->addWidget(warning, 0, Qt::AlignTop);
        conflictLayout->addWidget(m_conflictText, 1);
        m_conflict->hide();
        l->addWidget(m_conflict);

        auto* defaults = new QHBoxLayout();
        defaults->addWidget(m_defaultCaption);
        defaults->addStretch();
        defaults->addWidget(m_defaultKeys);
        l->addLayout(defaults);
        l->addWidget(m_btnBox);

        connect(m_keySeqEdit, &ShortcutWidget::shortcutChanged, this, [this] { updateConflict(); });
        connect(m_keySeqEdit, &ShortcutWidget::shortcutReset, this, [this] { updateConflict(); });

        Vault::followTheme(this, [this, warning] {
            const auto p = FluentTheme::palette();
            Vault::styleLabel(m_intro, Typography::FontRole::Body, p.textSecondary);
            Vault::styleLabel(m_defaultCaption, Typography::FontRole::Body, p.textSecondary);
            Vault::styleLabel(m_conflictText, Typography::FontRole::Body, p.text);
            QFont mono(FluentTheme::monoFontFamily());
            mono.setPixelSize(22);
            mono.setWeight(QFont::DemiBold);
            m_keySeqEdit->setFont(mono);
            // A large inset well, outlined in the accent while it listens for keys
            m_keySeqEdit->setStyleSheet(
                QStringLiteral(
                    "QLineEdit { background-color: %1; color: %2; border: 1px solid %3; border-radius: 16px; "
                    "padding: 0 12px; } QLineEdit:focus { border: 2px solid %4; }")
                    .arg(p.inset.name(QColor::HexArgb),
                         p.text.name(QColor::HexArgb),
                         p.strokeStrong.name(QColor::HexArgb),
                         p.accent.name(QColor::HexArgb)));
            auto fieldPalette = m_keySeqEdit->palette();
            fieldPalette.setColor(QPalette::PlaceholderText, p.textTertiary);
            m_keySeqEdit->setPalette(fieldPalette);
            QFont small(FluentTheme::monoFontFamily());
            small.setPixelSize(12);
            small.setWeight(QFont::DemiBold);
            m_defaultKeys->setFont(small);
            m_defaultKeys->setColors(p.stroke, p.text);
            QFont glyph(Typography::FontFamily::FluentIcons);
            glyph.setPixelSize(16);
            warning->setFont(glyph);
            auto palette = warning->palette();
            palette.setColor(QPalette::WindowText, p.caution);
            warning->setPalette(palette);
        });
        setInitialFocus(m_keySeqEdit);
        setFocusProxy(m_keySeqEdit);
    }

    QKeySequence keySequence() const
    {
        return m_keySeqEdit->sequence();
    }

    bool shouldRestoreDefault() const
    {
        return m_restoreDefault;
    }

    // Whether the note about another action using the keys was shown when the user saved
    bool sawConflict() const
    {
        return !m_conflict->isHidden();
    }

private:
    void updateConflict()
    {
        const auto name = m_conflictName ? m_conflictName(keySequence()) : QString();
        m_conflictText->setText(QObject::tr("%1 uses this already. Saving takes it from there.").arg(name));
        m_conflict->setVisible(!name.isEmpty());
    }

    void restoreDefault(QAbstractButton* btn)
    {
        if (m_btnBox->standardButton(btn) == QDialogButtonBox::RestoreDefaults) {
            m_restoreDefault = true;
            reject();
        }
    }

    bool m_restoreDefault = false;
    ShortcutWidget* const m_keySeqEdit;
    QDialogButtonBox* const m_btnBox;
    QLabel* const m_intro;
    VaultPanel* const m_conflict;
    QLabel* const m_conflictText;
    QLabel* const m_defaultCaption;
    VaultPill* const m_defaultKeys;
    ConflictName m_conflictName;
};

class ShortcutSettingsWidget final : public QWidget
{
public:
    explicit ShortcutSettingsWidget(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_tableView(new QTableView(this))
        , m_filterLineEdit(new VaultLineEdit(this))
        , m_resetShortcutsButton(new VaultButton(QObject::tr("Reset Shortcuts"), this))
    {
        auto h = new QHBoxLayout();
        h->addWidget(m_filterLineEdit);
        h->addWidget(m_resetShortcutsButton);
        h->setStretch(0, 1);

        auto l = new QVBoxLayout(this);
        l->addWidget(new QLabel(QObject::tr("Double click an action to change its shortcut")));
        l->addLayout(h);
        l->addWidget(m_tableView);

        m_model.setColumnCount(2);
        m_model.setHorizontalHeaderLabels({QObject::tr("Action"), QObject::tr("Shortcuts")});

        m_proxy.setFilterKeyColumn(-1);
        m_proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);
        m_proxy.setSourceModel(&m_model);

        m_filterLineEdit->setPlaceholderText(QObject::tr("Filter..."));
        m_filterLineEdit->setContentMargins({12, 10, 12, 10});
        m_resetShortcutsButton->setFixedHeight(m_filterLineEdit->sizeHint().height());
        connect(m_filterLineEdit, &QLineEdit::textChanged, &m_proxy, &QSortFilterProxyModel::setFilterFixedString);

        connect(m_resetShortcutsButton, &QPushButton::clicked, this, [this]() {
            auto ac = ActionCollection::instance();
            for (auto action : ac->actions()) {
                action->setShortcut(ac->defaultShortcut(action));
            }
            loadSettings();
        });

        m_tableView->setModel(&m_proxy);
        m_tableView->setSortingEnabled(true);
        m_tableView->sortByColumn(0, Qt::AscendingOrder);
        m_tableView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_tableView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_tableView->verticalHeader()->hide();
        m_tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tableView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);

        connect(m_tableView, &QTableView::doubleClicked, this, &ShortcutSettingsWidget::onDoubleClicked);
    }

    void loadSettings()
    {
        m_changedActions.clear();
        m_filterLineEdit->clear();
        m_model.setRowCount(0);
        const auto& actions = ActionCollection::instance()->actions();
        for (auto a : actions) {
            auto name = a->toolTip().isEmpty() ? acceleratorsStrippedText(a->text()) : a->toolTip();
            auto col1 = new QStandardItem(name);
            col1->setData(QVariant::fromValue(a), Qt::UserRole);
            auto col2 = new QStandardItem(a->shortcut().toString());
            m_model.appendRow({col1, col2});
        }
    }

    void saveSettings()
    {
        if (m_changedActions.count()) {
            for (const auto& action : m_changedActions.keys()) {
                action->setShortcut(m_changedActions.value(action));
            }
            ActionCollection::instance()->saveShortcuts();
        }
        m_changedActions.clear();
        m_filterLineEdit->clear();
    }

private:
    static QString acceleratorsStrippedText(QString text)
    {
        for (int i = 0; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('&') && i + 1 < text.size() && text.at(i + 1) != QLatin1Char('&')) {
                text.remove(i, 1);
            }
        }
        return text;
    }

    QString actionLabel(QAction* action) const
    {
        return action->toolTip().isEmpty() ? acceleratorsStrippedText(action->text()) : action->toolTip();
    }

    // The action already using `change`, counting the changes made here but not saved yet
    QAction* findConflict(QAction* action, const QKeySequence& change, bool* hasConflict) const
    {
        *hasConflict = false;
        auto conflict = ActionCollection::instance()->isConflictingShortcut(action, change);
        if (conflict) {
            // we conflicted with an action inside action collection
            // check if the conflicted action is updated here
            if (!m_changedActions.contains(conflict) || m_changedActions.value(conflict) == change) {
                *hasConflict = true;
            }
        } else if (!change.isEmpty()) {
            // we did not conflict with any shortcut inside action collection
            // check if we conflict with any locally modified action
            for (auto chAction : m_changedActions.keys()) {
                if (chAction != action && m_changedActions.value(chAction) == change) {
                    *hasConflict = true;
                    conflict = chAction;
                    break;
                }
            }
        }
        return conflict;
    }

    void onDoubleClicked(QModelIndex index)
    {
        if (index.column() != 0) {
            index = index.sibling(index.row(), 0);
        }
        index = m_proxy.mapToSource(index);
        auto action = index.data(Qt::UserRole).value<QAction*>();

        const auto actionName =
            action->toolTip().isEmpty() ? acceleratorsStrippedText(action->text()) : action->toolTip();
        const auto current = m_changedActions.contains(action) ? m_changedActions.value(action) : action->shortcut();
        KeySequenceDialog dialog(
            actionName,
            current,
            ActionCollection::instance()->defaultShortcut(action),
            [this, action](const QKeySequence& sequence) {
                bool conflicting = false;
                auto* other = findConflict(action, sequence, &conflicting);
                return conflicting ? actionLabel(other) : QString();
            },
            this);
        int ret = dialog.exec();

        QKeySequence change;
        if (ret == QDialog::Accepted) {
            change = dialog.keySequence();
        } else if (dialog.shouldRestoreDefault()) {
            change = ActionCollection::instance()->defaultShortcut(action);
        } else {
            // Rejected
            return;
        }

        bool hasConflict = false;
        auto* conflict = findConflict(action, change, &hasConflict);

        // Saving over the note the dialog showed takes the keys without asking again
        if (hasConflict && ret == QDialog::Accepted && dialog.sawConflict()) {
            m_changedActions[conflict] = {};
            for (auto item : m_model.findItems(change.toString(), Qt::MatchExactly, 1)) {
                item->setText("");
            }
            hasConflict = false;
        }

        if (hasConflict) {
            auto conflictName = actionLabel(conflict);
            auto conflictSeq = change.toString();

            auto ans = MessageBox::question(
                this,
                QObject::tr("Shortcut Conflict"),
                QObject::tr("Shortcut %1 conflicts with '%2'. Overwrite shortcut?").arg(conflictSeq, conflictName),
                MessageBox::Overwrite | MessageBox::Discard,
                MessageBox::Discard);
            if (ans == MessageBox::Discard) {
                // Bail out before making any changes
                return;
            }

            // Reset the conflict shortcut
            m_changedActions[conflict] = {};
            for (auto item : m_model.findItems(conflictSeq, Qt::MatchExactly, 1)) {
                item->setText("");
            }
        }

        m_changedActions[action] = change;
        auto item = m_model.itemFromIndex(index.sibling(index.row(), 1));
        item->setText(change.toString());
    }

    QTableView* m_tableView;
    VaultLineEdit* m_filterLineEdit;
    VaultButton* m_resetShortcutsButton;
    QStandardItemModel m_model;
    QSortFilterProxyModel m_proxy;
    QHash<QAction*, QKeySequence> m_changedActions;
};

QString ShortcutSettingsPage::name()
{
    return QObject::tr("Shortcuts");
}

QIcon ShortcutSettingsPage::icon()
{
    return icons()->icon("keyboard-shortcuts");
}

QWidget* ShortcutSettingsPage::createWidget()
{
    return new ShortcutSettingsWidget();
}

void ShortcutSettingsPage::loadSettings(QWidget* widget)
{
    static_cast<ShortcutSettingsWidget*>(widget)->loadSettings();
}

void ShortcutSettingsPage::saveSettings(QWidget* widget)
{
    static_cast<ShortcutSettingsWidget*>(widget)->saveSettings();
}
