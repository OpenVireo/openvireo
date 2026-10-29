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

#ifndef KEEPASSXC_BUILDINFO_H
#define KEEPASSXC_BUILDINFO_H

#include <QCoreApplication>
#include <QString>

#include <optional>

/**
 * What kind of build this is, for the build pill and the pre-release notice.
 *
 * Decided from the build type, the update channel, the version and whether Qt's debug mode is on, so a nightly
 * reads as a nightly and a debug build of a release tag still reads as a development build.
 */
class BuildInfo
{
    Q_DECLARE_TR_FUNCTIONS(BuildInfo)

public:
    enum class Kind
    {
        Stable,
        // A release before 1.0
        Beta,
        Nightly,
        Preview,
        Development
    };

    static Kind kind();
    static bool isStable();
    static bool isDebug();
    static QString version();
    static QString channel();

    // The pill's text, such as "Nightly"; empty for a stable build
    static QString shortLabel();
    // The pill's text spelled out for screen readers, such as "Nightly build"
    static QString accessibleName();
    static QString noticeTitle();
    static QString noticeMessage();

    static void setKindForTests(std::optional<Kind> kind);
};

#endif // KEEPASSXC_BUILDINFO_H
