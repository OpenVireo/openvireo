/*
 *  Copyright (C) 2012 Felix Geyer <debfx@fobos.de>
 *  Copyright (C) 2020 KeePassXC Team <team@keepassxc.org>
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

#include "WelcomeWidget.h"
#include "ui_WelcomeWidget.h"
#include <QKeyEvent>

#include "config-keepassx.h"
#include "core/Config.h"
#include "gui/Icons.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultWidgets.h"

#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>

namespace
{
    // Recent database rows: a database tile, the file name, and its folder beneath
    class RecentDatabaseDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            Q_UNUSED(index)
            return {option.rect.width(), 56};
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto p = FluentTheme::palette();
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
            const QRectF row = QRectF(option.rect).adjusted(4, 2, -4, -2);

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            if (selected || hovered) {
                QPainterPath path;
                path.addRoundedRect(row, 4, 4);
                painter->fillPath(path, selected && hovered ? p.subtlePressed : p.subtleHover);
            }
            if (selected && option.state.testFlag(QStyle::State_HasFocus)) {
                QPainterPath pill;
                pill.addRoundedRect(QRectF(row.left(), row.center().y() - 8, 3, 16), 1.5, 1.5);
                painter->fillPath(pill, p.accent);
            }

            const QRectF tile(row.left() + 12, row.center().y() - 16, 32, 32);
            QPainterPath tilePath;
            tilePath.addRoundedRect(tile, 6, 6);
            painter->fillPath(tilePath, p.accentSubtle);
            QFont glyphFont(Typography::FontFamily::FluentIcons);
            glyphFont.setPixelSize(16);
            painter->setFont(glyphFont);
            painter->setPen(p.accentText);
            painter->drawText(tile, Qt::AlignCenter, Typography::Icons::Storage);

            const QFileInfo file(index.data(Qt::DisplayRole).toString());
            const QRectF text(tile.right() + 12, row.top() + 8, row.right() - tile.right() - 24, row.height() - 16);
            const auto nameFont = Vault::font(Typography::FontRole::BodyStrong);
            const auto folderFont = Vault::font(Typography::FontRole::Caption);
            painter->setFont(nameFont);
            painter->setPen(p.text);
            painter->drawText(text,
                              Qt::AlignLeft | Qt::AlignTop,
                              QFontMetrics(nameFont).elidedText(file.fileName(), Qt::ElideRight, int(text.width())));
            painter->setFont(folderFont);
            painter->setPen(p.textSecondary);
            painter->drawText(
                text,
                Qt::AlignLeft | Qt::AlignBottom,
                QFontMetrics(folderFont)
                    .elidedText(QDir::toNativeSeparators(file.absolutePath()), Qt::ElideMiddle, int(text.width())));
            painter->restore();
        }
    };
} // namespace

WelcomeWidget::WelcomeWidget(QWidget* parent)
    : QWidget(parent)
    , m_ui(new Ui::WelcomeWidget())
{
    m_ui->setupUi(this);

    m_ui->welcomeLabel->setText(tr("Welcome to KeePassXC %1").arg(KEEPASSXC_VERSION));
    QFont welcomeLabelFont = m_ui->welcomeLabel->font();
    welcomeLabelFont.setBold(true);
    welcomeLabelFont.setPointSize(welcomeLabelFont.pointSize() + 4);
    m_ui->welcomeLabel->setFont(welcomeLabelFont);

    m_ui->iconLabel->setPixmap(icons()->applicationIcon().pixmap(64));
    m_ui->buttonNewDatabase->setIcon(icons()->icon("document-new"));
    m_ui->buttonNewDatabase->setStyleSheet("text-align:center;");
    m_ui->buttonOpenDatabase->setIcon(icons()->icon("document-open"));
    m_ui->buttonOpenDatabase->setStyleSheet("text-align:center;");
    m_ui->buttonImport->setIcon(icons()->icon("document-import"));
    m_ui->buttonImport->setStyleSheet("text-align:center;");

    if (FluentTheme::vaultLayoutEnabled()) {
        setupVaultStyle();
    }

    refreshLastDatabases();

    connect(m_ui->buttonNewDatabase, SIGNAL(clicked()), SIGNAL(newDatabase()));
    connect(m_ui->buttonOpenDatabase, SIGNAL(clicked()), SIGNAL(openDatabase()));
    connect(m_ui->buttonImport, SIGNAL(clicked()), SIGNAL(importFile()));
    connect(m_ui->recentListWidget,
            SIGNAL(itemActivated(QListWidgetItem*)),
            this,
            SLOT(openDatabaseFromFile(QListWidgetItem*)));
}

WelcomeWidget::~WelcomeWidget() = default;

void WelcomeWidget::setupVaultStyle()
{
    Vault::paintCanvas(this);

    // Fluent glyphs, with creating a database as the primary action
    const QList<std::pair<VaultButton*, QString>> buttons = {{m_ui->buttonNewDatabase, Typography::Icons::Add},
                                                             {m_ui->buttonOpenDatabase, Typography::Icons::Folder},
                                                             {m_ui->buttonImport, Typography::Icons::Download}};
    for (const auto& [button, glyph] : buttons) {
        button->setStyleSheet({});
        button->setIcon({});
        button->setIconGlyph(glyph);
        button->setFluentLayout(fluent::basicinput::Button::IconBefore);
    }
    m_ui->buttonNewDatabase->setFluentStyle(fluent::basicinput::Button::Accent);

    auto* recent = m_ui->recentListWidget;
    recent->setItemDelegate(new RecentDatabaseDelegate(recent));
    recent->setMouseTracking(true);
    recent->setAttribute(Qt::WA_MacShowFocusRect, false);
    recent->viewport()->setAttribute(Qt::WA_Hover);
    recent->viewport()->setAutoFillBackground(false);
    recent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_vaultStyle = true;

    Vault::followTheme(this, [this] {
        const auto p = FluentTheme::palette();
        Vault::styleLabel(m_ui->welcomeLabel, Typography::FontRole::Title, p.text);
        Vault::styleLabel(m_ui->startLabel, Typography::FontRole::Body, p.textSecondary);
        Vault::styleLabel(m_ui->recentLabel, Typography::FontRole::BodyStrong, p.text);
        // The recent databases sit on a card
        m_ui->recentListWidget->setStyleSheet(
            QStringLiteral("QListWidget { background-color: %1; border: 1px solid %2; border-radius: 8px;"
                           " padding: 4px; }")
                .arg(p.layer.name(QColor::HexArgb), p.stroke.name(QColor::HexArgb)));
    });
}

void WelcomeWidget::openDatabaseFromFile(QListWidgetItem* item)
{
    if (!item || item->text().isEmpty()) {
        return;
    }
    emit openDatabaseFile(item->text());
}

void WelcomeWidget::removeFromLastDatabases(QListWidgetItem* item)
{
    if (!item || item->text().isEmpty()) {
        return;
    }

    if (config()->get(Config::RememberLastDatabases).toBool()) {
        QStringList lastDatabases = config()->get(Config::LastDatabases).toStringList();
        lastDatabases.removeOne(item->text());
        config()->set(Config::LastDatabases, lastDatabases);
    }
    refreshLastDatabases();
}

void WelcomeWidget::refreshLastDatabases()
{
    m_ui->recentListWidget->clear();
    const QStringList lastDatabases = config()->get(Config::LastDatabases).toStringList();
    for (const QString& database : lastDatabases) {
        auto itm = new QListWidgetItem;
        itm->setText(database);
        m_ui->recentListWidget->addItem(itm);
    }

    if (m_vaultStyle) {
        // Room for up to four databases on the card; more scroll
        const int rows = qBound(1, m_ui->recentListWidget->count(), 4);
        m_ui->recentListWidget->setFixedHeight(rows * 56 + 12);
    }

    bool recent_visibility = (m_ui->recentListWidget->count() > 0);
    m_ui->startLabel->setVisible(!recent_visibility);
    m_ui->recentListWidget->setVisible(recent_visibility);
    m_ui->recentLabel->setVisible(recent_visibility);
}

void WelcomeWidget::keyPressEvent(QKeyEvent* event)
{
    if (m_ui->recentListWidget->hasFocus()) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            openDatabaseFromFile(m_ui->recentListWidget->currentItem());
        } else if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            removeFromLastDatabases(m_ui->recentListWidget->currentItem());
        }
    }

    QWidget::keyPressEvent(event);
}

void WelcomeWidget::showEvent(QShowEvent* event)
{
    refreshLastDatabases();
    QWidget::showEvent(event);
}
