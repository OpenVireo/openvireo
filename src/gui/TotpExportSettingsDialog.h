/*
 *  Copyright (C) 2017 KeePassXC Team <team@keepassxc.org>
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

#ifndef KEEPASSX_TotpExportSettingsDialog_H
#define KEEPASSX_TotpExportSettingsDialog_H

#include "core/Database.h"
#include "gui/DatabaseWidget.h"
#include "gui/vault/VaultDialog.h"

class QVBoxLayout;
class SquareSvgWidget;
class QLabel;
class QDialogButtonBox;
class VaultInfoBar;

// Entry › TOTP › QR code: the entry's TOTP setup as a QR code to scan into another authenticator, closing by itself
// after a while. Over the main window it is a card, with the QR code on white so it scans in the dark theme too.
class TotpExportSettingsDialog : public VaultDialog
{
    Q_OBJECT

public:
    explicit TotpExportSettingsDialog(DatabaseWidget* parent = nullptr, Entry* entry = nullptr);
    ~TotpExportSettingsDialog() override;

private slots:
    void copyToClipboard();
    void autoClose();

private:
    void applyColors();

    int m_secTillClose;
    QString m_totpUri;
    QTimer* m_timer;

    QVBoxLayout* m_verticalLayout;
    QStackedWidget* m_totpSvgContainerWidget;
    SquareSvgWidget* m_totpSvgWidget;
    QLabel* m_intro;
    QLabel* m_countDown;
    VaultInfoBar* m_warning;
    QDialogButtonBox* m_buttonBox;
};

#endif // KEEPASSX_TOTPEXPORTSETTINGSDIALOG_H
