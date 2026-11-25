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

#ifndef OPENVIREO_VAULTCOLORFLYOUT_H
#define OPENVIREO_VAULTCOLORFLYOUT_H

#include <QColor>
#include <QPointer>
#include <QWidget>

#include <functional>

class QAbstractButton;
class QLabel;
class QLineEdit;

/**
 * A flyout under a colour button, in place of the stock colour dialog: a grid of swatches, a hex field, More… for
 * the system's colour dialog, Use default to clear the colour and Done. Each pick applies at once, so whatever the
 * colour shows on updates while the flyout is open.
 */
class VaultColorFlyout : public QWidget
{
    Q_OBJECT

public:
    enum Swatches
    {
        // Strong colours that read as text on the list
        TextColors,
        // Light tints that keep text readable on top of them
        Tints
    };

    // Opens the flyout under `anchor`. `picked` gets each colour picked, and an invalid colour for Use default.
    static VaultColorFlyout* open(QWidget* anchor,
                                  const QColor& current,
                                  Swatches swatches,
                                  const QString& title,
                                  std::function<void(const QColor&)> picked);

    QColor color() const;

    // The colours of a set of swatches, for tests
    static QList<QColor> swatchColors(Swatches swatches);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    VaultColorFlyout(QWidget* anchor,
                     const QColor& current,
                     Swatches swatches,
                     const QString& title,
                     std::function<void(const QColor&)> picked);

    void pick(const QColor& color);
    void place();
    void applyColors();

    QPointer<QWidget> m_anchor;
    QColor m_color;
    std::function<void(const QColor&)> m_picked;
    QLabel* m_title;
    QLabel* m_hexLabel;
    QLineEdit* m_hex;
    QList<QAbstractButton*> m_swatches;
};

#endif // OPENVIREO_VAULTCOLORFLYOUT_H
