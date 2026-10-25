/*
 *  Copyright (C) 2025 KeePassXC Team <team@keepassxc.org>
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

#include "UpdateChecker.h"

#include "NetworkManager.h"
#include "config-keepassx.h"
#include "core/Clock.h"
#include "core/Config.h"

#include <QNetworkReply>

const QString UpdateChecker::ErrorVersion("error");
UpdateChecker* UpdateChecker::m_instance(nullptr);

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
    , m_reply(nullptr)
    , m_isManuallyRequested(false)
{
}

UpdateChecker::~UpdateChecker()
{
}

void UpdateChecker::checkForUpdates(bool manuallyRequested)
{
    // Skip update if we are already performing one
    if (m_reply) {
        return;
    }

    auto nextCheck = config()->get(Config::GUI_CheckForUpdatesNextCheck).toULongLong();
    m_isManuallyRequested = manuallyRequested;

    if (m_isManuallyRequested || Clock::currentSecondsSinceEpoch() >= nextCheck) {
        m_bytesReceived.clear();

        // The feed is a GitHub release asset, which redirects to its storage host
        QNetworkRequest request(UpdateFeed::feedUrl(UpdateFeed::channel()));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setRawHeader("Accept", "application/rss+xml, application/xml");

        m_reply = getNetMgr()->get(request);

        connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::fetchFinished);
        connect(m_reply, &QIODevice::readyRead, this, &UpdateChecker::fetchReadyRead);
    }
}

void UpdateChecker::fetchReadyRead()
{
    m_bytesReceived += m_reply->readAll();
}

void UpdateChecker::fetchFinished()
{
    bool error = (m_reply->error() != QNetworkReply::NoError);
    bool hasNewVersion = false;
    QUrl url = m_reply->url();
    QString version = "";

    m_reply->deleteLater();
    m_reply = nullptr;

    if (!error) {
        m_latestRelease = UpdateFeed::latestRelease(m_bytesReceived);
        if (m_latestRelease.isValid()) {
            version = m_latestRelease.displayVersion;
            // Builds outside every channel have no update version and are never offered an update
            hasNewVersion = UpdateFeed::isEnabled()
                            && UpdateFeed::compareVersions(KEEPASSXC_UPDATE_VERSION, m_latestRelease.version) < 0;
        } else {
            version = ErrorVersion;
        }

        // Check again in 7 days
        config()->set(Config::GUI_CheckForUpdatesNextCheck, Clock::currentDateTime().addDays(7).toSecsSinceEpoch());
    } else {
        version = ErrorVersion;
    }

    emit updateCheckFinished(hasNewVersion, version, m_isManuallyRequested);
}

UpdateFeed::Release UpdateChecker::latestRelease() const
{
    return m_latestRelease;
}

UpdateChecker* UpdateChecker::instance()
{
    if (!m_instance) {
        m_instance = new UpdateChecker();
    }

    return m_instance;
}
