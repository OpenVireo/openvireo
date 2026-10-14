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

#include "AutofillProtocol.h"

#include <QJsonDocument>
#include <QtEndian>

namespace AutofillProtocol
{
    QByteArray encodeFrame(const QJsonObject& message)
    {
        const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
        QByteArray frame(4, '\0');
        qToBigEndian<quint32>(static_cast<quint32>(body.size()), frame.data());
        frame.append(body);
        return frame;
    }

    QJsonObject success(const QJsonObject& fields)
    {
        auto response = fields;
        response.insert("ok", true);
        return response;
    }

    QJsonObject failure(const char* error)
    {
        return QJsonObject({{"ok", false}, {"error", QString::fromLatin1(error)}});
    }

    void FrameReader::append(const QByteArray& data)
    {
        if (!m_error) {
            m_buffer.append(data);
        }
    }

    QJsonObject FrameReader::next()
    {
        if (m_error || m_buffer.size() < 4) {
            return {};
        }

        const auto length = qFromBigEndian<quint32>(m_buffer.constData());
        if (length == 0 || length > MaxFrameSize) {
            m_error = true;
            m_buffer.clear();
            return {};
        }
        if (static_cast<quint32>(m_buffer.size()) < 4 + length) {
            return {};
        }

        const auto body = m_buffer.mid(4, length);
        m_buffer.remove(0, 4 + length);

        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            m_error = true;
            m_buffer.clear();
            return {};
        }
        return document.object();
    }

    bool FrameReader::hasError() const
    {
        return m_error;
    }
} // namespace AutofillProtocol
