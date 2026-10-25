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

#include "gui/osutils/AutoUpdater.h"

#import <Sparkle/Sparkle.h>

// Sparkle reads the public key from SUPublicEDKey in Info.plist; the feed comes from here so that
// switching channels takes effect without a restart
@interface OVSparkleDelegate : NSObject <SPUUpdaterDelegate>
@property(copy) NSString* feedUrl;
@end

@implementation OVSparkleDelegate
- (nullable NSString*)feedURLStringForUpdater:(SPUUpdater*)updater
{
    Q_UNUSED(updater)
    return self.feedUrl;
}
@end

namespace
{
    class SparkleUpdater : public AutoUpdater
    {
    public:
        SparkleUpdater()
        {
            m_delegate = [[OVSparkleDelegate alloc] init];
            // The controller keeps only a weak reference to its delegate
            m_controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:NO
                                                                         updaterDelegate:m_delegate
                                                                      userDriverDelegate:nil];
        }

        void checkForUpdates() override
        {
            [m_controller checkForUpdates:nil];
        }

    protected:
        void startUpdater() override
        {
            [m_controller startUpdater];
        }

        void setFeedUrl(const QUrl& url) override
        {
            m_delegate.feedUrl = url.toString(QUrl::FullyEncoded).toNSString();
        }

        void setAutomaticChecks(bool enabled) override
        {
            // Setting this also stops Sparkle from asking for permission itself; the app asks instead
            m_controller.updater.automaticallyChecksForUpdates = enabled;
        }

    private:
        OVSparkleDelegate* m_delegate;
        SPUStandardUpdaterController* m_controller;
    };
} // namespace

AutoUpdater* createSparkleUpdater()
{
    return new SparkleUpdater();
}
