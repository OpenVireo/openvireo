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

#include "CoreTestServers.h"

#include <QThread>

namespace CoreTestServers
{
    namespace
    {
        // A test server stream, polled without blocking; waits a little between empty polls
        class Stream : public ov::ByteStream
        {
        public:
            explicit Stream(rust::Box<ov::TestStream> stream)
                : m_stream(std::move(stream))
            {
            }

            std::uint16_t status() const override
            {
                return m_stream->status();
            }

            bool next_chunk(rust::Vec<std::uint8_t>& chunk, ov::TransportFailure&) const override
            {
                for (;;) {
                    auto poll = m_stream->poll_chunk();
                    if (poll.kind == ov::TestStreamPollKind::Data) {
                        chunk = std::move(poll.data);
                        return true;
                    }
                    if (poll.kind == ov::TestStreamPollKind::End) {
                        return false;
                    }
                    QThread::msleep(5);
                }
            }

            void close() const override
            {
                m_stream->close();
            }

        private:
            rust::Box<ov::TestStream> m_stream;
        };
    } // namespace

    Transport::Transport(const ov::TestServers* servers)
        : m_servers(servers)
    {
    }

    ov::HttpResponse Transport::send(const ov::HttpRequest& req, ov::TransportFailure& failure) const
    {
        return m_servers->handle(req, failure);
    }

    std::unique_ptr<ov::ByteStream> Transport::open_stream(const ov::HttpRequest& req,
                                                           ov::TransportFailure& failure) const
    {
        auto stream = m_servers->open_stream(req, failure);
        if (failure.kind != ov::TransportErrorKind::None) {
            return nullptr;
        }
        return std::make_unique<Stream>(std::move(stream));
    }

    Clock::Clock(const ov::TestServers* servers)
        : m_servers(servers)
    {
    }

    std::int64_t Clock::now_unix_ms() const
    {
        return m_servers->now_ms();
    }
} // namespace CoreTestServers
