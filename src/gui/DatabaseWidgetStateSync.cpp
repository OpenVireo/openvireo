/*
 * Copyright (C) 2018 KeePassXC Team <team@keepassxc.org>
 * Copyright (C) 2014 Felix Geyer <debfx@fobos.de>
 * Copyright (C) 2014 Florian Geyer <blueice@fobos.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or (at your option)
 * version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "DatabaseWidgetStateSync.h"

#include <QCoreApplication>

DatabaseWidgetStateSync::DatabaseWidgetStateSync(QObject* parent)
    : QObject(parent)
    , m_activeDbWidget(nullptr)
    , m_blockUpdates(false)
{
    m_splitterSizes = {
        {Config::GUI_SplitterState, variantToIntList(config()->get(Config::GUI_SplitterState))},
        {Config::GUI_GroupSplitterState, variantToIntList(config()->get(Config::GUI_GroupSplitterState))},
        {Config::GUI_VaultSplitterState, variantToIntList(config()->get(Config::GUI_VaultSplitterState))}};

    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(100);
    connect(&m_syncTimer, &QTimer::timeout, this, &DatabaseWidgetStateSync::sync);
}

DatabaseWidgetStateSync::~DatabaseWidgetStateSync() = default;

/**
 * Sync state with persistent storage.
 */
void DatabaseWidgetStateSync::sync()
{
    m_syncTimer.stop();
    config()->set(Config::GUI_SplitterState, intListToVariant(m_splitterSizes.value(Config::GUI_SplitterState)));
    config()->set(Config::GUI_GroupSplitterState,
                  intListToVariant(m_splitterSizes.value(Config::GUI_GroupSplitterState)));
    config()->set(Config::GUI_VaultSplitterState,
                  intListToVariant(m_splitterSizes.value(Config::GUI_VaultSplitterState)));
    config()->sync();
}

void DatabaseWidgetStateSync::setActive(DatabaseWidget* dbWidget)
{
    if (m_activeDbWidget) {
        if (m_activeDbWidget->currentMode() != DatabaseWidget::Mode::LockedMode) {
            // Update settings from previously active database if unlocked
            updateAll();
        }
        disconnect(m_activeDbWidget, nullptr, this, nullptr);
    }

    m_activeDbWidget = dbWidget;

    if (m_activeDbWidget) {
        if (m_activeDbWidget->currentMode() != DatabaseWidget::Mode::LockedMode) {
            // Immediately apply settings to active database if already unlocked
            applySplitterSizes();
        }

        connect(m_activeDbWidget, SIGNAL(databaseAboutToUnlock()), SLOT(blockUpdates()));
        connect(m_activeDbWidget, SIGNAL(databaseUnlocked()), SLOT(applySplitterSizes()));
        connect(m_activeDbWidget, &DatabaseWidget::databaseLocked, this, [this] { updateAll(true); });
        connect(m_activeDbWidget, SIGNAL(splitterSizesChanged()), SLOT(updateSplitterSizes()));
    }
}

void DatabaseWidgetStateSync::applySplitterSizes()
{
    if (!m_activeDbWidget) {
        return;
    }

    m_blockUpdates = true;

    m_activeDbWidget->setSplitterSizes(m_splitterSizes);

    m_blockUpdates = false;
}

void DatabaseWidgetStateSync::blockUpdates()
{
    m_blockUpdates = true;
}

void DatabaseWidgetStateSync::updateAll(bool forceSync)
{
    updateSplitterSizes();
    if (forceSync) {
        sync();
    }
}

void DatabaseWidgetStateSync::updateSplitterSizes()
{
    if (!m_blockUpdates) {
        // Merge, since each layout reports only the splitters it uses
        const auto sizes = m_activeDbWidget->splitterSizes();
        for (auto itr = sizes.constBegin(); itr != sizes.constEnd(); ++itr) {
            m_splitterSizes.insert(itr.key(), itr.value());
        }
        m_syncTimer.start();
    }
}

QList<int> DatabaseWidgetStateSync::variantToIntList(const QVariant& variant)
{
    const QVariantList list = variant.toList();
    QList<int> result;

    for (const QVariant& var : list) {
        bool ok;
        int size = var.toInt(&ok);
        if (ok) {
            result.append(size);
        } else {
            result.clear();
            break;
        }
    }

    return result;
}

QVariant DatabaseWidgetStateSync::intListToVariant(const QList<int>& list)
{
    QVariantList result;

    for (int value : list) {
        result.append(value);
    }

    return result;
}
