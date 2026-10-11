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
#include "gui/DatabaseWidget.h"
#include "gui/remote/RemoteSettings.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <components/layout/Card.h>

#include <QDir>
#include <QFileInfo>
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

    QLabel* glyphLabel(const QString& glyph, int size, QWidget* parent)
    {
        auto* label = new QLabel(glyph, parent);
        QFont font(Typography::FontFamily::FluentIcons);
        font.setPixelSize(size);
        label->setFont(font);
        label->setAlignment(Qt::AlignCenter);
        return label;
    }

    void setLabelColor(QLabel* label, const QColor& color)
    {
        auto palette = label->palette();
        palette.setColor(QPalette::WindowText, color);
        label->setPalette(palette);
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
    , m_dbIcon(glyphLabel(Typography::Icons::Storage, Typography::IconSize::Large, this))
    , m_dbName(new QLabel(this))
    , m_dbDetail(new QLabel(this))
    , m_groupsLabel(new QLabel(tr("Groups"), this))
{
    setObjectName("vaultNavPane");
    setMinimumWidth(200);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 10);
    layout->setSpacing(2);

    // The open database
    auto* dbCard = new fluent::layout::Card(this);
    dbCard->setAppearance(fluent::layout::Card::Layer);
    dbCard->setBorderVisible(true);
    auto* dbLayout = new QHBoxLayout(dbCard);
    dbLayout->setContentsMargins(10, 6, 10, 6);
    dbLayout->setSpacing(10);
    m_dbIcon->setFixedWidth(24);
    dbLayout->addWidget(m_dbIcon);
    auto* dbText = new QVBoxLayout();
    dbText->setSpacing(0);
    dbText->addWidget(m_dbName);
    dbText->addWidget(m_dbDetail);
    dbLayout->addLayout(dbText, 1);
    layout->addWidget(dbCard);
    layout->addSpacing(8);

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

void VaultNavPane::setDatabase(const QSharedPointer<Database>& db)
{
    if (m_db) {
        m_db->disconnect(this);
    }
    m_db = db;
    if (m_db) {
        connect(m_db.data(), &Database::modified, this, &VaultNavPane::refresh);
        connect(m_db.data(), &Database::filePathChanged, this, &VaultNavPane::refresh);
        connect(m_db.data(), &Database::databaseOpened, this, &VaultNavPane::refresh);
    }
    refresh();
}

void VaultNavPane::refresh()
{
    if (!m_db) {
        m_dbName->clear();
        m_dbDetail->clear();
        return;
    }

    const QFileInfo file(m_db->filePath());
    auto name = m_db->metadata()->name();
    if (name.isEmpty()) {
        name = file.fileName();
    }
    m_dbName->setText(name);
    m_dbName->setToolTip(m_db->filePath());

    QStringList detail;
    if (!m_db->filePath().isEmpty()) {
        detail << homeRelative(file.absolutePath());
    }
    if (m_db->rootGroup()) {
        detail << tr("%n entries", "", m_db->rootGroup()->entriesRecursive().size());
    }
    m_dbDetail->setText(detail.join(QStringLiteral(" · ")));
}

void VaultNavPane::onThemeUpdated()
{
    const auto p = FluentTheme::palette();
    setLabelColor(m_dbIcon, p.accentText);
    Vault::styleLabel(m_dbName, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_dbDetail, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_groupsLabel, Typography::FontRole::Caption, p.textSecondary);
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

    m_title->setMinimumWidth(0);
    layout->addWidget(m_title);
    layout->addSpacing(10);
    layout->addWidget(m_count, 0, Qt::AlignBottom);
    layout->addStretch();

    auto* newEntry = new fluent::basicinput::Button(tr("New Entry"), this);
    newEntry->setObjectName("vaultNewEntryButton");
    newEntry->setFluentStyle(fluent::basicinput::Button::Accent);
    newEntry->setFluentLayout(fluent::basicinput::Button::IconBefore);
    newEntry->setIconGlyph(Typography::Icons::Add);
    Vault::bindAction(newEntry, "actionEntryNew");
    layout->addWidget(newEntry);
    layout->addSpacing(4);

    auto addCommand = [this, layout](const QString& glyph, const QString& toolTip, const QString& action) {
        auto* button = Vault::iconButton(glyph, toolTip, this);
        Vault::bindAction(button, action);
        layout->addWidget(button);
        return button;
    };
    addCommand(Typography::Icons::Edit, tr("Edit entry"), "actionEntryEdit");
    addCommand(Typography::Icons::Delete, tr("Delete entry"), "actionEntryDelete");

    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::VLine);
    separator->setFixedHeight(20);
    layout->addSpacing(4);
    layout->addWidget(separator);
    layout->addSpacing(4);

    addCommand(Typography::Icons::Lock, tr("Lock database"), "actionLockDatabase");

    // Less frequent commands, all mirrored from the main window menus
    auto* more = Vault::iconButton(Typography::Icons::More, tr("More commands"), this);
    auto* menu = new QMenu(more);
    for (const auto& name : {QStringLiteral("actionEntryClone"),
                             QStringLiteral("actionEntryDownloadIcon"),
                             QString(),
                             QStringLiteral("actionGroupNew"),
                             QStringLiteral("actionGroupEdit"),
                             QString(),
                             QStringLiteral("actionDatabaseSave"),
                             QStringLiteral("actionDatabaseSettings")}) {
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
    layout->addWidget(more);

    onThemeUpdated();
}

void VaultCommandBar::setTitle(const QString& title, int entryCount)
{
    m_title->setText(title);
    m_count->setText(tr("%n entries", "", entryCount));
}

void VaultCommandBar::onThemeUpdated()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_title, Typography::FontRole::Subtitle, p.text);
    Vault::styleLabel(m_count, Typography::FontRole::Caption, p.textSecondary);
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
        connect(m_dbWidget, &DatabaseWidget::databaseSyncInProgress, this, [this] {
            m_status[m_dbWidget].state = State::Syncing;
            refresh();
        });
        connect(m_dbWidget, &DatabaseWidget::databaseSyncCompleted, this, [this] {
            auto& status = m_status[m_dbWidget];
            status.state = State::Synced;
            status.lastSync = QDateTime::currentDateTime();
            status.error.clear();
            refresh();
        });
        connect(m_dbWidget, &DatabaseWidget::databaseSyncFailed, this, [this](const QString&, const QString& error) {
            auto& status = m_status[m_dbWidget];
            status.state = State::Failed;
            status.error = error;
            refresh();
        });
        connect(m_dbWidget, &DatabaseWidget::databaseModified, this, &VaultSyncChip::refresh);
        connect(m_dbWidget, &DatabaseWidget::databaseUnlocked, this, &VaultSyncChip::refresh);
        connect(m_dbWidget, &DatabaseWidget::databaseLocked, this, &VaultSyncChip::refresh);
        connect(m_dbWidget, &QObject::destroyed, this, [this](QObject* object) {
            m_status.remove(static_cast<DatabaseWidget*>(object));
        });
    }
    refresh();
}

QString VaultSyncChip::remoteName() const
{
    if (!m_dbWidget) {
        return {};
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

    const auto status = m_status.value(m_dbWidget);
    switch (status.state) {
    case State::Idle:
        setText(name);
        setToolTip(tr("Sync with %1").arg(name));
        break;
    case State::Syncing:
        setText(tr("Syncing…"));
        setToolTip(tr("Syncing with %1").arg(name));
        break;
    case State::Synced: {
        const auto minutes = status.lastSync.secsTo(QDateTime::currentDateTime()) / 60;
        setText(minutes < 1 ? tr("%1 · synced just now").arg(name)
                            : tr("%1 · synced %n min ago", "", int(minutes)).arg(name));
        setToolTip(tr("Last synced at %1. Click to sync again.")
                       .arg(QLocale().toString(status.lastSync.time(), QLocale::ShortFormat)));
        break;
    }
    case State::Failed:
        setText(tr("%1 · sync failed").arg(name));
        setToolTip(status.error.isEmpty() ? tr("Click to try again.") : status.error);
        break;
    }
    setAccessibleName(text());
    updateGeometry();
    update();
}

void VaultSyncChip::sync()
{
    if (!m_dbWidget || m_status.value(m_dbWidget).state == State::Syncing) {
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

    // Status dot: green when synced, red when the last sync failed
    QColor dot = p.textTertiary;
    switch (m_status.value(m_dbWidget).state) {
    case State::Synced:
        dot = p.success;
        break;
    case State::Failed:
        dot = p.critical;
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

VaultHeaderBar::VaultHeaderBar(QWidget* searchWidget, QWidget* parent)
    : QWidget(parent)
    , m_syncChip(new VaultSyncChip(this))
{
    setObjectName("vaultHeaderBar");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 4);
    layout->setSpacing(12);
    layout->addStretch(1);
    searchWidget->setParent(this);
    searchWidget->setMinimumWidth(240);
    searchWidget->setMaximumWidth(440);
    searchWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(searchWidget, 3);
    layout->addWidget(m_syncChip);
    layout->addStretch(1);
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
