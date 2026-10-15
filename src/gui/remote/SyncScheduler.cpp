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

#include "SyncScheduler.h"

#include "config-keepassx.h"

#include <QGuiApplication>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkInformation>
#endif

SyncScheduler::SyncScheduler(QObject* parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(DefaultSaveDelayMsec);
    connect(&m_saveTimer, &QTimer::timeout, this, &SyncScheduler::request);

    m_pollTimer.setInterval(DefaultPollIntervalMsec);
    connect(&m_pollTimer, &QTimer::timeout, this, &SyncScheduler::request);

    // Not available in GUI-less tests
    if (qGuiApp) {
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                notifyEvent();
            }
        });
    }

#ifdef KPXC_FEATURE_NETWORK
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability);
#else
    QNetworkInformation::load(QNetworkInformation::Feature::Reachability);
#endif
    if (auto* network = QNetworkInformation::instance()) {
        connect(network,
                &QNetworkInformation::reachabilityChanged,
                this,
                [this](QNetworkInformation::Reachability reachability) {
                    if (reachability == QNetworkInformation::Reachability::Online) {
                        notifyEvent();
                    }
                });
    }
#endif
}

void SyncScheduler::setEnabled(bool enabled)
{
    m_enabled = enabled;
    if (enabled) {
        m_pollTimer.start();
    } else {
        m_saveTimer.stop();
        m_pollTimer.stop();
    }
}

bool SyncScheduler::isEnabled() const
{
    return m_enabled;
}

void SyncScheduler::setSaveDelay(int msec)
{
    m_saveTimer.setInterval(msec);
}

void SyncScheduler::setPollInterval(int msec)
{
    m_pollTimer.setInterval(msec);
}

void SyncScheduler::setMinEventGap(int msec)
{
    m_minEventGapMsec = msec;
}

void SyncScheduler::notifySaved()
{
    if (m_enabled) {
        // Restarting the timer folds a burst of saves into one sync
        m_saveTimer.start();
    }
}

void SyncScheduler::requestNow()
{
    if (m_enabled) {
        request();
    }
}

void SyncScheduler::notifyEvent()
{
    if (m_enabled && (!m_sinceLastRequest.isValid() || m_sinceLastRequest.elapsed() >= m_minEventGapMsec)) {
        request();
    }
}

void SyncScheduler::request()
{
    m_saveTimer.stop();
    // The next poll counts from this sync, not from the last poll
    m_pollTimer.start();
    m_sinceLastRequest.start();
    emit syncRequested();
}
