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

#ifndef OPENVIREO_VAULTUNLOCKSHELL_H
#define OPENVIREO_VAULTUNLOCKSHELL_H

#include <QFrame>
#include <QList>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

/**
 * The three steps of an unlock in the dark panel: enter the password, make the key, open the vault.
 * They follow the real unlock; a caller that can't tell the last two apart stays on "make the key".
 */
class VaultUnlockSteps : public QFrame
{
    Q_OBJECT

public:
    struct Step
    {
        QString title;
        QString text;
    };

    explicit VaultUnlockSteps(QWidget* parent = nullptr);

    void setSteps(const QList<Step>& steps);
    // 0: waiting for the password, 1: making the key, 2: opening, 3: done. busy marks the current step as running.
    void setProgress(int step, bool busy);
    int progress() const;
    bool isBusy() const;
    void applyTheme();

private:
    struct Row
    {
        QWidget* row;
        QLabel* dot;
        QLabel* text;
    };

    QList<Step> m_steps;
    QList<Row> m_rows;
    QVBoxLayout* m_layout;
    int m_progress = 0;
    bool m_busy = false;
};

/**
 * The split unlock layout shared by the account lock screen and a locked database, matching the web app and the
 * account site's sign-in page: a dark panel with the logo, an eyebrow, a heading and the steps, and a column with
 * the form, a top-right link and a legal footer. Below 900px wide the panel hides and the column is enough.
 */
class VaultUnlockShell : public QWidget
{
    Q_OBJECT

public:
    explicit VaultUnlockShell(QWidget* parent = nullptr);

    void setHero(const QString& eyebrow, const QString& heading, const QString& text);
    VaultUnlockSteps* steps() const;
    // The form; the shell keeps it at most 440px wide, centred in the column
    void setContent(QWidget* content);
    // Top right of the column, e.g. "Not jdoe? Switch account"
    void setTopLink(QWidget* link);
    bool isHeroVisible() const;

    static constexpr int HeroBreakpoint = 900;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void applyTheme();

    QFrame* m_hero;
    QLabel* m_logo;
    QLabel* m_brand;
    QLabel* m_eyebrow;
    QLabel* m_heading;
    QLabel* m_heroText;
    VaultUnlockSteps* m_steps;
    QLabel* m_heroFoot;
    QHBoxLayout* m_topRow;
    QVBoxLayout* m_contentLayout;
    QList<QLabel*> m_legalLinks;
};

#endif // OPENVIREO_VAULTUNLOCKSHELL_H
