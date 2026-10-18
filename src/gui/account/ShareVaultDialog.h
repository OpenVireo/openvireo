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

#ifndef KEEPASSXC_SHAREVAULTDIALOG_H
#define KEEPASSXC_SHAREVAULTDIALOG_H

#include "account/VaultSharing.h"

#include <QDialog>
#include <QPointer>
#include <QScopedPointer>

namespace Ui
{
    class ShareVaultDialog;
}

/**
 * Who can open a vault: its members with their access and public-key fingerprints, sharing with another
 * account by email (after showing that account's fingerprint to compare), changing access, and removing
 * members, which rotates the vault key in the same step.
 */
class ShareVaultDialog : public QDialog
{
    Q_OBJECT

public:
    ShareVaultDialog(VaultSharing* sharing, const QString& vaultId, QWidget* parent = nullptr);
    ~ShareVaultDialog() override;

    QList<VaultSharing::Member> members() const;
    // Whether an operation is running
    bool isBusy() const;

public slots:
    void reload();

private slots:
    void lookup();
    void share();
    void changeRole();
    void removeSelected();
    void rotate();
    void updateButtons();

private:
    void setBusy(const QString& message);
    void finish(bool ok, const QString& error, const QString& success);
    QString selectedPrincipal() const;
    bool canManage() const;

    const QScopedPointer<Ui::ShareVaultDialog> m_ui;
    QPointer<VaultSharing> m_sharing;
    QString m_vaultId;
    QList<VaultSharing::Member> m_members;
    VaultSharing::Recipient m_recipient;
    bool m_busy = false;
};

#endif // KEEPASSXC_SHAREVAULTDIALOG_H
