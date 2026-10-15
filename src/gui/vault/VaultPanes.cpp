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

#include "VaultPanes.h"

#include "core/Database.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/Tools.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/Icons.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultWidgets.h"

#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    QString homeRelative(const QString& path)
    {
        const auto home = QDir::homePath();
        if (path.startsWith(home)) {
            return QStringLiteral("~") + path.mid(home.size());
        }
        return QDir::toNativeSeparators(path);
    }
} // namespace

void VaultCanvas::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), FluentTheme::palette().canvas);
}

void VaultSurface::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    constexpr qreal radius = 8;

    // Rounded top-left corner; the other edges run off the window
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, radius, radius);
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(path, p.surface);
    painter.setPen(QPen(p.stroke, 1));
    painter.drawPath(path);
}

VaultSectionHeader::VaultSectionHeader(const QString& text, QWidget* parent)
    : QAbstractButton(parent)
{
    setText(text);
    setAccessibleName(text);
    setCheckable(true);
    setChecked(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    Vault::followTheme(this, [this] { update(); });
}

QSize VaultSectionHeader::sizeHint() const
{
    // As tall as the Groups title, which has the same margins
    const QFontMetrics metrics(Vault::font(Typography::FontRole::Caption));
    return {metrics.horizontalAdvance(text()) + 52, metrics.height() + 12};
}

void VaultSectionHeader::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(4, 2, -4, 0);
    if (isDown() || underMouse()) {
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
    }
    if (hasFocus()) {
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        painter.setPen(QPen(p.text, 2));
        painter.drawPath(path);
    }

    // The text lines up with the Groups title, the chevron with the group tree's
    painter.setFont(Vault::font(Typography::FontRole::Caption));
    painter.setPen(p.textSecondary);
    const QRectF textRect = QRectF(rect()).adjusted(12, 8, -32, -4);
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text());

    QFont iconFont(Typography::FontFamily::FluentIcons);
    iconFont.setPixelSize(Typography::IconSize::Compact);
    painter.setFont(iconFont);
    painter.drawText(QRectF(r.right() - 24, textRect.top(), 20, textRect.height()),
                     Qt::AlignCenter,
                     isChecked() ? Typography::Icons::ChevronDown : Typography::Icons::ChevronRight);
}

VaultNavButton::VaultNavButton(const QString& glyph, const QString& text, QWidget* parent)
    : QAbstractButton(parent)
    , m_glyph(glyph)
{
    setText(text);
    setAccessibleName(text);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
}

QSize VaultNavButton::sizeHint() const
{
    return {QFontMetrics(Vault::font(Typography::FontRole::Body)).horizontalAdvance(text()) + 52, 36};
}

void VaultNavButton::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void VaultNavButton::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void VaultNavButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRectF r = QRectF(rect()).adjusted(4, 2, -4, -2);
    if (isDown() || m_hovered) {
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
    }
    if (hasFocus()) {
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        painter.setPen(QPen(p.text, 2));
        painter.drawPath(path);
    }

    const auto color = isEnabled() ? p.text : p.textTertiary;
    QFont iconFont(Typography::FontFamily::FluentIcons);
    iconFont.setPixelSize(Typography::IconSize::Standard);
    painter.setFont(iconFont);
    painter.setPen(isEnabled() ? p.textSecondary : p.textTertiary);
    painter.drawText(QRectF(r.left() + 8, r.top(), 20, r.height()), Qt::AlignCenter, m_glyph);

    painter.setFont(Vault::font(Typography::FontRole::Body));
    painter.setPen(color);
    painter.drawText(r.adjusted(40, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
}

VaultNavPane::VaultNavPane(QWidget* groups, QWidget* parent)
    : QWidget(parent)
    , m_groupsLabel(new QLabel(tr("Groups"), this))
{
    setObjectName("vaultNavPane");
    setMinimumWidth(200);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 10);
    layout->setSpacing(2);

    m_groupsLabel->setContentsMargins(12, 4, 12, 4);
    layout->addWidget(m_groupsLabel);
    layout->addWidget(groups, 1);

    // Tools that don't belong to one database
    auto* footer = new QWidget(this);
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 6, 0, 0);
    footerLayout->setSpacing(2);
    auto* generator = new VaultNavButton(Typography::Icons::Calculator, tr("Password Generator"), footer);
    Vault::bindAction(generator, "actionPasswordGenerator");
    footerLayout->addWidget(generator);
    auto* reports = new VaultNavButton(Typography::Icons::Shield, tr("Database Reports"), footer);
    Vault::bindAction(reports, "actionReports");
    footerLayout->addWidget(reports);
    auto* settings = new VaultNavButton(Typography::Icons::Settings, tr("Settings"), footer);
    Vault::bindAction(settings, "actionSettings");
    footerLayout->addWidget(settings);
    layout->addWidget(footer);

    onThemeUpdated();
}

void VaultNavPane::onThemeUpdated()
{
    Vault::styleLabel(m_groupsLabel, Typography::FontRole::Caption, FluentTheme::palette().textSecondary);
    update();
}

VaultCommandBar::VaultCommandBar(QWidget* parent)
    : QWidget(parent)
    , m_title(new QLabel(this))
    , m_count(new QLabel(this))
{
    setObjectName("vaultCommandBar");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(4);

    // The title and count share a baseline; onThemeUpdated() lowers the count by the difference in their ascents
    auto* heading = new QWidget(this);
    auto* headingLayout = new QHBoxLayout(heading);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(10);
    m_title->setMinimumWidth(0);
    m_title->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_count->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    headingLayout->addWidget(m_title, 0, Qt::AlignTop);
    headingLayout->addWidget(m_count, 0, Qt::AlignTop);
    layout->addWidget(heading, 0, Qt::AlignVCenter);
    layout->addStretch();

    // The entry commands, which Cancel and Save replace while an entry is edited
    m_commands = new QWidget(this);
    auto* commandsLayout = new QHBoxLayout(m_commands);
    commandsLayout->setContentsMargins(0, 0, 0, 0);
    commandsLayout->setSpacing(4);
    layout->addWidget(m_commands);

    auto* newEntry = new fluent::basicinput::Button(tr("New Entry"), m_commands);
    newEntry->setObjectName("vaultNewEntryButton");
    newEntry->setFluentStyle(fluent::basicinput::Button::Accent);
    newEntry->setFluentLayout(fluent::basicinput::Button::IconBefore);
    newEntry->setIconGlyph(Typography::Icons::Add);
    Vault::bindAction(newEntry, "actionEntryNew");
    commandsLayout->addWidget(newEntry);
    commandsLayout->addSpacing(4);

    auto addCommand = [this, commandsLayout](const QString& glyph, const QString& toolTip, const QString& action) {
        auto* button = Vault::iconButton(glyph, toolTip, m_commands);
        Vault::bindAction(button, action);
        commandsLayout->addWidget(button);
        return button;
    };
    addCommand(Typography::Icons::Keyboard, tr("Perform Auto-Type"), "actionEntryAutoTypeSequence");
    addCommand(Typography::Icons::Edit, tr("Edit entry"), "actionEntryEdit");
    addCommand(Typography::Icons::Delete, tr("Delete entry"), "actionEntryDelete");

    auto* separator = new QFrame(m_commands);
    separator->setFrameShape(QFrame::VLine);
    separator->setFixedHeight(20);
    commandsLayout->addSpacing(4);
    commandsLayout->addWidget(separator);
    commandsLayout->addSpacing(4);

    addCommand(Typography::Icons::Lock, tr("Lock database"), "actionLockDatabase");

    // Less frequent commands, all mirrored from the main window menus
    auto* more = Vault::iconButton(Typography::Icons::More, tr("More commands"), m_commands);
    auto* menu = new VaultMenu(more);
    for (const auto& name : {QStringLiteral("actionEntryClone"),
                             QStringLiteral("actionEntryDownloadIcon"),
                             QString(),
                             QStringLiteral("actionGroupNew"),
                             QStringLiteral("actionGroupEdit"),
                             QString(),
                             QStringLiteral("actionDatabaseSave")}) {
        if (name.isEmpty()) {
            menu->addSeparator();
        } else if (auto* action = FluentTheme::action(name)) {
            menu->addAction(action);
        }
    }
    connect(more, &QAbstractButton::clicked, this, [more, menu] {
        menu->popup(more->mapToGlobal(QPoint(more->width() - menu->sizeHint().width(), more->height())));
    });
    more->setVisible(!menu->isEmpty());
    commandsLayout->addWidget(more);

    // Cancel and Save for the entry being edited, which click the editor's own buttons
    m_editCommands = new QWidget(this);
    m_editCommands->hide();
    auto* editLayout = new QHBoxLayout(m_editCommands);
    editLayout->setContentsMargins(0, 0, 0, 0);
    editLayout->setSpacing(4);
    layout->addWidget(m_editCommands);

    m_cancelButton = new fluent::basicinput::Button(tr("Cancel"), m_editCommands);
    m_cancelButton->setObjectName("vaultEditCancelButton");
    m_cancelButton->setFluentLayout(fluent::basicinput::Button::IconBefore);
    m_cancelButton->setIconGlyph(Typography::Icons::Cancel);
    connect(m_cancelButton, &QAbstractButton::clicked, this, [this] {
        if (auto* button = editorButton({QDialogButtonBox::Cancel, QDialogButtonBox::Close})) {
            button->click();
        }
    });
    editLayout->addWidget(m_cancelButton);

    // Save is styled like New Entry, which it stands in for
    m_saveButton = new fluent::basicinput::Button(tr("Save"), m_editCommands);
    m_saveButton->setObjectName("vaultEditSaveButton");
    m_saveButton->setFluentStyle(fluent::basicinput::Button::Accent);
    m_saveButton->setFluentLayout(fluent::basicinput::Button::IconBefore);
    m_saveButton->setIconGlyph(Typography::Icons::Save);
    connect(m_saveButton, &QAbstractButton::clicked, this, [this] {
        if (auto* button = editorButton({QDialogButtonBox::Ok})) {
            button->click();
        }
    });
    editLayout->addWidget(m_saveButton);

    onThemeUpdated();
}

void VaultCommandBar::setTitle(const QString& title, int entryCount)
{
    m_title->setText(title);
    m_count->setText(tr("%n entries", "", entryCount));
}

void VaultCommandBar::setEditorButtons(QDialogButtonBox* buttons)
{
    m_editorButtons = buttons;
    m_commands->setVisible(!buttons);
    m_editCommands->setVisible(buttons);
    if (!buttons) {
        return;
    }
    // A read-only entry, such as a history entry, can only be closed
    const bool readOnly = !buttons->button(QDialogButtonBox::Ok);
    m_saveButton->setVisible(!readOnly);
    const auto cancelText = readOnly ? tr("Close") : tr("Cancel");
    m_cancelButton->setText(cancelText);
}

QAbstractButton* VaultCommandBar::editorButton(std::initializer_list<QDialogButtonBox::StandardButton> which) const
{
    if (!m_editorButtons) {
        return nullptr;
    }
    for (auto standardButton : which) {
        if (auto* button = m_editorButtons->button(standardButton)) {
            return button;
        }
    }
    return nullptr;
}

void VaultCommandBar::onThemeUpdated()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_title, Typography::FontRole::Subtitle, p.text);
    Vault::styleLabel(m_count, Typography::FontRole::Caption, p.textSecondary);
    const int baselineOffset = QFontMetrics(m_title->font()).ascent() - QFontMetrics(m_count->font()).ascent();
    m_count->setContentsMargins(0, qMax(0, baselineOffset), 0, 0);
}

VaultSyncChip::VaultSyncChip(QWidget* parent)
    : QAbstractButton(parent)
    , m_relativeTimeTimer(new QTimer(this))
{
    setObjectName("vaultSyncChip");
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setVisible(false);

    // Keep "synced 2 min ago" current
    m_relativeTimeTimer->setInterval(30 * 1000);
    connect(m_relativeTimeTimer, &QTimer::timeout, this, &VaultSyncChip::refresh);
    m_relativeTimeTimer->start();

    connect(this, &QAbstractButton::clicked, this, &VaultSyncChip::sync);
}

void VaultSyncChip::setDatabaseWidget(DatabaseWidget* dbWidget)
{
    if (m_dbWidget == dbWidget) {
        refresh();
        return;
    }
    if (m_dbWidget) {
        m_dbWidget->disconnect(this);
    }
    m_dbWidget = dbWidget;

    if (m_dbWidget) {
        connect(m_dbWidget, &DatabaseWidget::syncStatusChanged, this, &VaultSyncChip::refresh);
        connect(m_dbWidget, &DatabaseWidget::databaseModified, this, &VaultSyncChip::refresh);
        connect(m_dbWidget, &DatabaseWidget::databaseUnlocked, this, &VaultSyncChip::refresh);
        connect(m_dbWidget, &DatabaseWidget::databaseLocked, this, &VaultSyncChip::refresh);
    }
    refresh();
}

QString VaultSyncChip::remoteName() const
{
    if (!m_dbWidget) {
        return {};
    }
    if (const auto* params = m_dbWidget->autoSyncParams()) {
        return params->name;
    }
    const auto params = m_dbWidget->getRemoteParams();
    // Prefer a sync server over command-based remotes
    for (const auto* param : params) {
        if (param->type == RemoteParams::Type::SyncServer) {
            return param->name;
        }
    }
    return params.isEmpty() ? QString() : params.first()->name;
}

void VaultSyncChip::refresh()
{
    const auto name = remoteName();
    const bool available = m_dbWidget && !m_dbWidget->isLocked() && !name.isEmpty();
    setVisible(available);
    if (!available) {
        return;
    }

    using State = DatabaseWidget::SyncStatus::State;
    const auto status = m_dbWidget->syncStatus();
    const auto automatic = status.automatic ? tr("Syncs automatically. ") : QString();
    switch (status.state) {
    case State::Idle:
        setText(name);
        setToolTip(automatic + tr("Sync with %1").arg(name));
        break;
    case State::Syncing:
        setText(tr("Syncing…"));
        setToolTip(tr("Syncing with %1").arg(name));
        break;
    case State::Synced: {
        const auto minutes = status.lastSync.secsTo(QDateTime::currentDateTime()) / 60;
        setText(minutes < 1 ? tr("%1 · synced just now").arg(name)
                            : tr("%1 · synced %n min ago", "", int(minutes)).arg(name));
        setToolTip(automatic
                   + tr("Last synced at %1. Click to sync again.")
                         .arg(QLocale().toString(status.lastSync.time(), QLocale::ShortFormat)));
        break;
    }
    case State::Failed:
        setText(tr("%1 · sync failed").arg(name));
        setToolTip(status.error.isEmpty() ? tr("Click to try again.") : status.error);
        break;
    case State::Offline:
        setText(tr("%1 · offline").arg(name));
        setToolTip(tr("%1\nRetrying automatically. Click to try now.").arg(status.error));
        break;
    case State::NeedsUnlock:
        setText(tr("%1 · unlock needed").arg(name));
        setToolTip(tr("The database on the server uses a different password or key file. Click to unlock it."));
        break;
    }
    setAccessibleName(text());
    updateGeometry();
    update();
}

void VaultSyncChip::sync()
{
    if (!m_dbWidget || m_dbWidget->syncStatus().state == DatabaseWidget::SyncStatus::State::Syncing) {
        return;
    }
    const auto name = remoteName();
    for (const auto* params : m_dbWidget->getRemoteParams()) {
        if (params->name == name) {
            m_dbWidget->syncWithRemote(params);
            return;
        }
    }
}

QSize VaultSyncChip::sizeHint() const
{
    const QFontMetrics metrics(Vault::font(Typography::FontRole::Caption));
    return {metrics.horizontalAdvance(text()) + 34, 28};
}

void VaultSyncChip::onThemeUpdated()
{
    update();
}

void VaultSyncChip::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void VaultSyncChip::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void VaultSyncChip::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, r.height() / 2, r.height() / 2);
    painter.fillPath(path, m_hovered || isDown() ? p.layer : p.surface);
    painter.setPen(QPen(hasFocus() ? p.text : p.stroke, hasFocus() ? 2 : 1));
    painter.drawPath(path);

    // Status dot: green when synced, red when the last sync failed, amber when it needs attention
    using State = DatabaseWidget::SyncStatus::State;
    QColor dot = p.textTertiary;
    switch (m_dbWidget ? m_dbWidget->syncStatus().state : State::Idle) {
    case State::Synced:
        dot = p.success;
        break;
    case State::Failed:
        dot = p.critical;
        break;
    case State::Offline:
    case State::NeedsUnlock:
        dot = p.caution;
        break;
    case State::Syncing:
        dot = p.accent;
        break;
    case State::Idle:
        break;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(dot);
    painter.drawEllipse(QPointF(r.left() + 14, r.center().y()), 3.5, 3.5);

    painter.setFont(Vault::font(Typography::FontRole::Caption));
    painter.setPen(p.textSecondary);
    painter.drawText(r.adjusted(24, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
}

VaultDatabasePicker::VaultDatabasePicker(DatabaseTabWidget* tabs, QWidget* parent)
    : QAbstractButton(parent)
    , m_tabs(tabs)
{
    setObjectName("vaultDatabasePicker");
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    connect(this, &QAbstractButton::clicked, this, &VaultDatabasePicker::showDatabaseMenu);
    connect(tabs, &DatabaseTabWidget::activeDatabaseChanged, this, &VaultDatabasePicker::setDatabaseWidget);
    connect(tabs, &DatabaseTabWidget::tabNameChanged, this, &VaultDatabasePicker::refresh);
    connect(tabs, &DatabaseTabWidget::databaseLocked, this, &VaultDatabasePicker::refresh);
    connect(tabs, &DatabaseTabWidget::databaseUnlocked, this, &VaultDatabasePicker::refresh);
    connect(tabs, &DatabaseTabWidget::databaseClosed, this, &VaultDatabasePicker::refresh);
    setDatabaseWidget(tabs->currentDatabaseWidget());
}

void VaultDatabasePicker::setDatabaseWidget(DatabaseWidget* dbWidget)
{
    if (m_dbWidget) {
        m_dbWidget->disconnect(this);
        if (m_dbWidget->database()) {
            m_dbWidget->database()->disconnect(this);
        }
    }
    m_dbWidget = dbWidget;
    if (m_dbWidget) {
        // The entry count and name follow the database; a reload replaces the database
        auto connectDatabase = [this](const QSharedPointer<Database>& db) {
            if (db) {
                connect(db.data(), &Database::modified, this, &VaultDatabasePicker::refresh);
                connect(db.data(), &Database::filePathChanged, this, &VaultDatabasePicker::refresh);
            }
        };
        connectDatabase(m_dbWidget->database());
        connect(m_dbWidget,
                &DatabaseWidget::databaseReplaced,
                this,
                [this, connectDatabase](const QSharedPointer<Database>& oldDb, const QSharedPointer<Database>& newDb) {
                    if (oldDb) {
                        oldDb->disconnect(this);
                    }
                    connectDatabase(newDb);
                    refresh();
                });
    }
    refresh();
}

void VaultDatabasePicker::refresh()
{
    if (!m_dbWidget || !m_dbWidget->database()) {
        setText({});
        m_detail.clear();
        m_locked = false;
        setToolTip({});
        updateGeometry();
        update();
        return;
    }

    const auto db = m_dbWidget->database();
    m_locked = m_dbWidget->isLocked();
    setText(m_dbWidget->displayName());

    QStringList detail;
    if (m_locked) {
        detail << tr("Locked");
    } else if (db->rootGroup()) {
        detail << tr("%n entries", "", db->rootGroup()->entriesRecursive().size());
    }
    if (!db->filePath().isEmpty()) {
        detail << homeRelative(QFileInfo(db->filePath()).absolutePath());
    }
    m_detail = detail.join(QStringLiteral(" · "));

    const int open = m_tabs ? m_tabs->count() : 0;
    setToolTip(open > 1 ? tr("%1\n%n databases open", "", open).arg(db->filePath()) : db->filePath());
    updateGeometry();
    update();
}

void VaultDatabasePicker::showDatabaseMenu()
{
    if (!m_tabs) {
        return;
    }

    VaultMenu menu(tr("Databases"), this);
    // Before adding actions: QMenu sizes its item rects, and so the area it repaints on hover, as they are added
    menu.setMinimumWidth(width());
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* dbWidget = m_tabs->databaseWidgetFromIndex(i);
        auto* action = menu.addAction(Tools::escapeAccelerators(m_tabs->tabName(i)));
        action->setCheckable(true);
        action->setChecked(i == m_tabs->currentIndex());
        if (dbWidget && dbWidget->isLocked()) {
            action->setIcon(icons()->icon("database-lock"));
        }
        action->setToolTip(dbWidget ? dbWidget->database()->filePath() : QString());
        connect(action, &QAction::triggered, this, [this, i] {
            if (m_tabs) {
                m_tabs->setCurrentIndex(i);
            }
        });
    }

    menu.addSeparator();
    for (const auto* name : {"actionDatabaseNew", "actionDatabaseOpen"}) {
        if (auto* action = FluentTheme::action(name)) {
            menu.addAction(action);
        }
    }
    menu.addSeparator();
    if (auto* action = FluentTheme::action("actionDatabaseSettings")) {
        menu.addAction(action);
    }
    menu.addSeparator();
    for (const auto* name : {"actionLockAllDatabases", "actionDatabaseClose"}) {
        if (auto* action = FluentTheme::action(name)) {
            menu.addAction(action);
        }
    }

    m_menuOpen = true;
    update();
    menu.exec(mapToGlobal(QPoint(0, height() + 2)));
    m_menuOpen = false;
    update();
}

QSize VaultDatabasePicker::sizeHint() const
{
    const QFontMetrics nameMetrics(Vault::font(Typography::FontRole::BodyStrong));
    const QFontMetrics detailMetrics(Vault::font(Typography::FontRole::Caption));
    const int textWidth = qMax(nameMetrics.horizontalAdvance(text()), detailMetrics.horizontalAdvance(m_detail));
    // Tile, text and chevron; long paths elide
    return {qBound(200, textWidth + 82, 320), 42};
}

void VaultDatabasePicker::onThemeUpdated()
{
    update();
}

void VaultDatabasePicker::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void VaultDatabasePicker::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void VaultDatabasePicker::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 6, 6);
    if (m_menuOpen || isDown()) {
        painter.fillPath(path, p.subtlePressed);
    } else if (m_hovered) {
        painter.fillPath(path, p.subtleHover);
    }
    if (hasFocus()) {
        painter.setPen(QPen(p.text, 2));
        painter.drawPath(path);
    }

    // Database tile, showing a lock while the database is locked
    const QRectF tile(r.left() + 8, r.center().y() - 14, 28, 28);
    QPainterPath tilePath;
    tilePath.addRoundedRect(tile, 6, 6);
    painter.fillPath(tilePath, p.accentSubtle);
    QFont glyphFont(Typography::FontFamily::FluentIcons);
    glyphFont.setPixelSize(16);
    painter.setFont(glyphFont);
    painter.setPen(p.accentText);
    painter.drawText(tile, Qt::AlignCenter, m_locked ? Typography::Icons::Lock : Typography::Icons::Storage);

    // Chevron
    const QRectF chevron(r.right() - 26, r.top(), 18, r.height());
    glyphFont.setPixelSize(10);
    painter.setFont(glyphFont);
    painter.setPen(p.textSecondary);
    painter.drawText(chevron, Qt::AlignCenter, Typography::Icons::ChevronDown);

    // Name over path and entry count
    const QRectF textRect(tile.right() + 10, r.top() + 4, chevron.left() - tile.right() - 14, r.height() - 8);
    const auto nameFont = Vault::font(Typography::FontRole::BodyStrong);
    const auto detailFont = Vault::font(Typography::FontRole::Caption);
    const QFontMetrics nameMetrics(nameFont);
    const QFontMetrics detailMetrics(detailFont);
    const int width = qRound(textRect.width());
    painter.setFont(nameFont);
    painter.setPen(p.text);
    painter.drawText(textRect.adjusted(0, 0, 0, -textRect.height() / 2),
                     Qt::AlignLeft | Qt::AlignBottom,
                     nameMetrics.elidedText(text(), Qt::ElideRight, width));
    painter.setFont(detailFont);
    painter.setPen(p.textSecondary);
    painter.drawText(textRect.adjusted(0, textRect.height() / 2, 0, 0),
                     Qt::AlignLeft | Qt::AlignTop,
                     detailMetrics.elidedText(m_detail, Qt::ElideMiddle, width));
}

VaultHeaderBar::VaultHeaderBar(DatabaseTabWidget* tabs, QWidget* searchWidget, QWidget* parent)
    : QWidget(parent)
    , m_databasePicker(new VaultDatabasePicker(tabs, this))
    , m_syncChip(new VaultSyncChip(this))
{
    setObjectName("vaultHeaderBar");

    // Picker on the left, sync status on the right, search centered between equal side columns
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(12, 8, 12, 4);
    layout->setHorizontalSpacing(12);
    layout->addWidget(m_databasePicker, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

    searchWidget->setParent(this);
    searchWidget->setMinimumWidth(240);
    searchWidget->setMaximumWidth(440);
    searchWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* center = new QHBoxLayout();
    center->addStretch(1);
    center->addWidget(searchWidget, 100);
    center->addStretch(1);
    layout->addLayout(center, 0, 1);
    layout->addWidget(m_syncChip, 0, 2, Qt::AlignRight | Qt::AlignVCenter);

    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 3);
    layout->setColumnStretch(2, 1);
}

VaultDatabasePicker* VaultHeaderBar::databasePicker() const
{
    return m_databasePicker;
}

VaultSyncChip* VaultHeaderBar::syncChip() const
{
    return m_syncChip;
}

void VaultHeaderBar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), FluentTheme::palette().canvas);
}
