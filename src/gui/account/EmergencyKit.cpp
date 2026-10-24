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

#include "EmergencyKit.h"

#include <QDateTime>
#include <QFile>
#include <QLocale>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextDocument>

EmergencyKit::EmergencyKit(const QString& email, const QString& server, const QString& secretKey)
    : m_email(email)
    , m_server(server)
    , m_secretKey(secretKey)
{
}

QString EmergencyKit::html() const
{
    const auto created = QLocale().toString(QDate::currentDate(), QLocale::LongFormat);
    const auto row = [](const QString& label, const QString& value) {
        return "<tr><td><b>" + label.toHtmlEscaped() + "</b></td><td>" + value + "</td></tr>";
    };
    QString html;
    html += "<h1>" + tr("OpenVireo Emergency Kit").toHtmlEscaped() + "</h1>";
    html += "<p>"
            + tr("Created %1. Print it or keep it somewhere safe, away from your devices.").arg(created).toHtmlEscaped()
            + "</p>";
    html += "<table cellpadding='6'>";
    html += row(tr("Email"), m_email.toHtmlEscaped());
    html += row(tr("Server"), m_server.toHtmlEscaped());
    html += row(tr("Secret Key"),
                "<span style='font-family: monospace; font-size: 16pt'>" + m_secretKey.toHtmlEscaped() + "</span>");
    html += row(tr("Master password"), "____________________________________");
    html += "</table>";
    html += "<h3>" + tr("Why you need it").toHtmlEscaped() + "</h3><ul>";
    for (const auto& point :
         {tr("Signing in on a new device takes your master password and this Secret Key."),
          tr("Your vaults are encrypted with both, on your devices. The server has neither and can't reset them: if "
             "you lose both this kit and every signed-in device, your vaults can't be recovered."),
          tr("Write your master password on the line by hand if you want to; OpenVireo never stores it.")}) {
        html += "<li>" + point.toHtmlEscaped() + "</li>";
    }
    html += "</ul><p><small>"
            + tr("Never share this kit or enter the Secret Key anywhere but in OpenVireo.").toHtmlEscaped()
            + "</small></p>";
    return html;
}

bool EmergencyKit::writePdf(const QString& filePath, QString* error) const
{
    {
        // The file is complete once the writer is gone
        QPdfWriter writer(filePath);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setTitle(tr("OpenVireo Emergency Kit"));
        writer.setCreator(QStringLiteral("OpenVireo"));
        QTextDocument document;
        document.setHtml(html());
        document.print(&writer);
    }
    if (QFile(filePath).size() == 0) {
        if (error) {
            *error = tr("Could not write %1.").arg(filePath);
        }
        return false;
    }
    return true;
}
