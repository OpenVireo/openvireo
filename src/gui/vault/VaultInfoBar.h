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

#ifndef OPENVIREO_VAULTINFOBAR_H
#define OPENVIREO_VAULTINFOBAR_H

#include <QFrame>
#include <QIcon>
#include <QList>
#include <QPointer>

class QAbstractButton;
class QHBoxLayout;
class QLabel;
class QPainter;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;

/**
 * A message bar in the look of a WinUI InfoBar: a severity icon and fill, an optional bold title, the message
 * (rich text, links), optional action buttons and a close button, inside a rounded card with a hairline stroke.
 *
 * It slides open and shut unless motion is turned off (high contrast, setAnimate(false)), can hide itself after a
 * timeout, and wraps its text to the width the layout gives it. Actions added with QWidget::addAction() become
 * buttons. Fluent-Qt's own InfoBar always reserves a title line, has a large minimum height and a fixed width,
 * which doesn't suit the short inline messages forms show.
 */
class VaultInfoBar : public QFrame
{
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText)
    Q_PROPERTY(QString title READ title WRITE setTitle)
    Q_PROPERTY(bool wordWrap READ wordWrap WRITE setWordWrap)
    Q_PROPERTY(bool closeButtonVisible READ isCloseButtonVisible WRITE setCloseButtonVisible)
    Q_PROPERTY(MessageType messageType READ messageType WRITE setMessageType)
    Q_PROPERTY(QIcon icon READ icon WRITE setIcon)

public:
    enum MessageType
    {
        Positive,
        Information,
        Warning,
        Error
    };
    Q_ENUM(MessageType)

    static const int DefaultAutoHideTimeout;
    static const int LongAutoHideTimeout;
    static const int DisableAutoHide;

    explicit VaultInfoBar(QWidget* parent = nullptr);
    explicit VaultInfoBar(const QString& text, QWidget* parent = nullptr);
    ~VaultInfoBar() override;

    QString text() const;
    QString title() const;
    MessageType messageType() const;
    bool wordWrap() const;
    bool isCloseButtonVisible() const;
    QIcon icon() const;
    int autoHideTimeout() const;
    // Show and hide at once instead of sliding
    void setAnimate(bool animate);

    bool isShowAnimationRunning() const;
    bool isHideAnimationRunning() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

    // The round severity badge at the start of a bar, also used by the notification centre
    static void paintStatusBadge(QPainter& painter, const QRectF& rect, MessageType type);
    // Open http(s) links with the system's handler; other links are ignored
    static void openHttpUrl(const QString& url);

public slots:
    void showMessage(const QString& text, VaultInfoBar::MessageType type);
    void showMessage(const QString& text, VaultInfoBar::MessageType type, int autoHideTimeout);
    void hideMessage();
    void setAutoHideTimeout(int autoHideTimeout);

    void setText(const QString& text);
    void setTitle(const QString& title);
    void setWordWrap(bool wordWrap);
    void setCloseButtonVisible(bool visible);
    void setMessageType(VaultInfoBar::MessageType type);
    // Replaces the severity badge; a null icon brings it back
    void setIcon(const QIcon& icon);

    void animatedShow();
    void animatedHide();

signals:
    void linkActivated(const QString& link);
    void linkHovered(const QString& link);
    void showAnimationStarted();
    void showAnimationFinished();
    void hideAnimationStarted();
    void hideAnimationFinished();

protected:
    void actionEvent(QActionEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    class Content;
    class Icon;

    void applyColors();
    void rebuildButtons();
    void updateContentGeometry();
    // The full height of the bar at a width
    int contentHeight(int width) const;
    bool shouldAnimate() const;
    void startAnimation(bool show);
    void finishAnimation();

    Content* m_content;
    QHBoxLayout* m_buttonLayout;
    Icon* m_icon;
    QVBoxLayout* m_iconColumn;
    QVBoxLayout* m_textColumn;
    QLabel* m_titleLabel;
    QLabel* m_textLabel;
    QAbstractButton* m_closeButton;
    QList<QPointer<QAbstractButton>> m_actionButtons;
    QTimer* m_autoHideTimer;
    QVariantAnimation* m_animation;
    QIcon m_customIcon;
    MessageType m_type = Information;
    int m_autoHideTimeout;
    bool m_animate = true;
    bool m_showing = false;
    bool m_wordWrap = true;
    int m_lastHintHeight = -1;
};

#endif // OPENVIREO_VAULTINFOBAR_H
