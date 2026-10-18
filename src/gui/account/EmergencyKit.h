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

#ifndef KEEPASSXC_EMERGENCYKIT_H
#define KEEPASSXC_EMERGENCYKIT_H

#include <QCoreApplication>
#include <QString>

/**
 * The Emergency Kit: a one-page PDF with what a new device needs to sign in, the account's email,
 * server and Secret Key (SPEC §2), and a blank line for the master password, which KeePassXC never
 * writes down.
 */
class EmergencyKit
{
    Q_DECLARE_TR_FUNCTIONS(EmergencyKit)

public:
    EmergencyKit(const QString& email, const QString& server, const QString& secretKey);

    QString html() const;
    bool writePdf(const QString& filePath, QString* error = nullptr) const;

private:
    QString m_email;
    QString m_server;
    QString m_secretKey;
};

#endif // KEEPASSXC_EMERGENCYKIT_H
