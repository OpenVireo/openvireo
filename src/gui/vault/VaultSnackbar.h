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

#ifndef OPENVIREO_VAULTSNACKBAR_H
#define OPENVIREO_VAULTSNACKBAR_H

#include "gui/vault/VaultInfoBar.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QWidget>

class QAbstractButton;
class QHBoxLayout;
class QLabel;
class QTimer;
class QVariantAnimation;

/**
 * A short message that floats over the bottom centre of its parent instead of pushing the content down, such as
 * "Merged the selected database" or a failed save. A dark card in the light theme and a light one in the dark theme,
 * with a status badge, the message, optional action buttons (QWidget::addAction) and a close button.
 *
 * It slides up from the bottom edge, hides itself after the timeout with a line that counts down along its bottom
 * edge, and waits while the pointer is over it. The parent's layout doesn't manage it; it follows the parent's size.
 * Takes the place of a VaultInfoBar above the vault, with the same calls.
 */
class VaultSnackbar : public QWidget
{
    Q_OBJECT

public:
    static const int MaximumWidth;
    static const int Margin;

    explicit VaultSnackbar(QWidget* parent);
    ~VaultSnackbar() override;

    QString text() const;
    VaultInfoBar::MessageType messageType() const;
    int autoHideTimeout() const;
    bool isCloseButtonVisible() const;
    // Show and hide at once instead of sliding
    void setAnimate(bool animate);
    bool isAnimating() const;
    // Fraction of the timeout still to go, 1 when it doesn't hide itself
    qreal remainingFraction() const;

    QSize sizeHint() const override;

public slots:
    void showMessage(const QString& text,
                     VaultInfoBar::MessageType type,
                     int autoHideTimeout = VaultInfoBar::DefaultAutoHideTimeout);
    // Hide at once
    void hideMessage();
    void animatedHide();
    void setCloseButtonVisible(bool visible);

signals:
    void linkActivated(const QString& link);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void actionEvent(QActionEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    class TextButton;

    void applyColors();
    void rebuildButtons();
    void reposition();
    bool shouldAnimate() const;
    void startAnimation(bool show);
    void pauseTimer();
    void resumeTimer();
    int cardWidth() const;
    int restingY() const;

    QHBoxLayout* m_layout;
    QLabel* m_textLabel;
    QHBoxLayout* m_buttonLayout;
    TextButton* m_closeButton;
    QList<QPointer<QAbstractButton>> m_actionButtons;
    QTimer* m_hideTimer;
    QTimer* m_tick;
    QElapsedTimer m_elapsed;
    QVariantAnimation* m_animation;
    VaultInfoBar::MessageType m_type = VaultInfoBar::Information;
    int m_autoHideTimeout = VaultInfoBar::DefaultAutoHideTimeout;
    // Time left when the pointer paused the countdown
    int m_remaining = 0;
    // 0 below the bottom edge, 1 resting in place
    qreal m_slide = 1;
    bool m_animate = true;
    bool m_showing = false;
};

#endif // OPENVIREO_VAULTSNACKBAR_H
