/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2014 Felix Geyer <debfx@fobos.de>
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

#ifndef KEEPASSX_URLEDIT_H
#define KEEPASSX_URLEDIT_H

#include <QAction>
#include <QPointer>

#include "core/Entry.h"
#include "gui/vault/VaultControls.h"

class URLEdit : public VaultLineEdit
{
    Q_OBJECT

public:
    explicit URLEdit(QWidget* parent = nullptr);
    void enableVerifyMode();
    void setEntry(Entry* entry);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void updateStylesheet(const QString& url = {});

private:
    QPointer<QAction> m_errorAction;
    bool m_invalid = false;
    QPointer<Entry> m_entry;
};

#endif // KEEPASSX_URLEDIT_H
