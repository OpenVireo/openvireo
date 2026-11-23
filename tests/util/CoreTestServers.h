/*
 *  Copyright (C) 2026 Bongani Mbigi
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

#ifndef OPENVIREO_CORETESTSERVERS_H
#define OPENVIREO_CORETESTSERVERS_H

#include <ovcore.h>

/*
 * openvireo-core's in-process auth and sync servers (ov::TestServers, feature test-server) plugged into the
 * desktop's Host: the HTTP transport and the clock go to the servers, the keychain and the offline cache stay
 * CoreHost's. Tests then run the real protocol without a network.
 */
namespace CoreTestServers
{
    class Transport : public ov::HttpTransport
    {
    public:
        explicit Transport(const ov::TestServers* servers);
        ov::HttpResponse send(const ov::HttpRequest& req, ov::TransportFailure& failure) const override;
        std::unique_ptr<ov::ByteStream> open_stream(const ov::HttpRequest& req,
                                                    ov::TransportFailure& failure) const override;

    private:
        const ov::TestServers* m_servers;
    };

    class Clock : public ov::Clock
    {
    public:
        explicit Clock(const ov::TestServers* servers);
        std::int64_t now_unix_ms() const override;

    private:
        const ov::TestServers* m_servers;
    };
} // namespace CoreTestServers

#endif // OPENVIREO_CORETESTSERVERS_H
