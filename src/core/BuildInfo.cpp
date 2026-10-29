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

#include "BuildInfo.h"

#include "config-keepassx.h"
#include "git-info.h"

#include <QRegularExpression>

namespace
{
    std::optional<BuildInfo::Kind> g_kindForTests;

    BuildInfo::Kind detectKind()
    {
#if defined(KEEPASSXC_BUILD_TYPE_RELEASE)
        if (BuildInfo::isDebug()) {
            return BuildInfo::Kind::Development;
        }
        return BuildInfo::version().startsWith(QLatin1String("0.")) ? BuildInfo::Kind::Beta : BuildInfo::Kind::Stable;
#else
        if (BuildInfo::isDebug()) {
            return BuildInfo::Kind::Development;
        }
        if (BuildInfo::channel() == QLatin1String("nightly")) {
            return BuildInfo::Kind::Nightly;
        }
        static const QRegularExpression preview(QStringLiteral("-(alpha|beta|rc|preview)\\d*$"),
                                                QRegularExpression::CaseInsensitiveOption);
        if (preview.match(BuildInfo::version()).hasMatch()) {
            return BuildInfo::Kind::Preview;
        }
        return BuildInfo::Kind::Development;
#endif
    }
} // namespace

BuildInfo::Kind BuildInfo::kind()
{
    if (g_kindForTests) {
        return *g_kindForTests;
    }
    static const Kind detected = detectKind();
    return detected;
}

bool BuildInfo::isStable()
{
    return kind() == Kind::Stable;
}

bool BuildInfo::isDebug()
{
#ifdef QT_NO_DEBUG
    return false;
#else
    return true;
#endif
}

QString BuildInfo::version()
{
    return QStringLiteral(KEEPASSXC_VERSION);
}

QString BuildInfo::channel()
{
    return QStringLiteral(KEEPASSXC_UPDATE_CHANNEL);
}

QString BuildInfo::shortLabel()
{
    switch (kind()) {
    case Kind::Beta:
        return tr("Beta");
    case Kind::Nightly:
        return tr("Nightly");
    case Kind::Preview:
        return tr("Preview");
    case Kind::Development:
#ifdef KEEPASSXC_BUILD_TYPE_RELEASE
        return tr("Debug");
#else
        return tr("Dev build");
#endif
    case Kind::Stable:
        break;
    }
    return {};
}

QString BuildInfo::accessibleName()
{
    switch (kind()) {
    case Kind::Beta:
        return tr("Beta build");
    case Kind::Nightly:
        return tr("Nightly build");
    case Kind::Preview:
        return tr("Preview build");
    case Kind::Development:
        return tr("Development build");
    case Kind::Stable:
        break;
    }
    return {};
}

QString BuildInfo::noticeTitle()
{
    switch (kind()) {
    case Kind::Beta:
        return tr("You're using a beta");
    case Kind::Nightly:
        return tr("You're using a nightly build");
    case Kind::Preview:
        return tr("You're using a preview");
    case Kind::Development:
        return tr("You're using a development build");
    case Kind::Stable:
        break;
    }
    return {};
}

QString BuildInfo::noticeMessage()
{
    switch (kind()) {
    case Kind::Beta:
        return tr("OpenVireo %1 is a beta and may still have bugs. Keep a backup of your databases, and report any "
                  "problems you find.")
            .arg(version());
    case Kind::Nightly:
        return tr("OpenVireo %1 is an untested weekly build. Keep a backup of your databases, and report any problems "
                  "you find.")
            .arg(version());
    case Kind::Preview:
        return tr("OpenVireo %1 is a preview of the next release and may contain bugs. Keep a backup of your "
                  "databases.")
            .arg(version());
    case Kind::Development:
        return tr("This build of OpenVireo (%1, revision %2) is for testing changes and isn't meant for everyday use. "
                  "Keep a backup of your databases.")
            .arg(version(), QStringLiteral(GIT_HEAD).left(7));
    case Kind::Stable:
        break;
    }
    return {};
}

void BuildInfo::setKindForTests(std::optional<Kind> kind)
{
    g_kindForTests = kind;
}
