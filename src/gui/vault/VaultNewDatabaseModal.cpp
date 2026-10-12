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

#include "VaultNewDatabaseModal.h"

#include "core/Database.h"
#include "core/Global.h"
#include "core/Group.h"
#include "gui/dbsettings/DatabaseSettingsWidgetDatabaseKey.h"
#include "gui/dbsettings/DatabaseSettingsWidgetEncryption.h"
#include "gui/dbsettings/DatabaseSettingsWidgetMetaDataSimple.h"
#include "gui/vault/VaultControls.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>

#include <functional>

namespace
{
    constexpr int StepListWidth = 220;
    constexpr int StepRowHeight = 52;
    constexpr int StepMarkerSize = 26;
    constexpr int StepMarkerLeft = 12;
} // namespace

namespace Vault
{
    // Numbered steps down the sidebar: finished ones carry a check, the current one is
    // outlined in the accent color. A finished step can be clicked to go back to it.
    class NewDatabaseSteps : public QWidget
    {
    public:
        NewDatabaseSteps(QStringList titles, QWidget* parent)
            : QWidget(parent)
            , m_titles(std::move(titles))
        {
            setFixedWidth(StepListWidth);
            setMouseTracking(true);
            setAccessibleName(VaultNewDatabaseModal::tr("Steps"));
        }

        void setCurrent(int current)
        {
            m_current = current;
            setAccessibleDescription(VaultNewDatabaseModal::tr("Step %1 of %2: %3")
                                         .arg(current + 1)
                                         .arg(m_titles.size())
                                         .arg(m_titles.value(current)));
            update();
        }

        std::function<void(int)> onStepClicked;

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            const auto colors = VaultModal::colors();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);

            for (int i = 0; i < m_titles.size(); ++i) {
                const QRect row = rowRect(i);
                const bool done = i < m_current;
                const bool current = i == m_current;

                if (done && i == m_hovered) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(colors.hover);
                    painter.drawRoundedRect(QRectF(row), 6, 6);
                }

                const QRectF marker(row.left() + StepMarkerLeft,
                                    row.top() + (row.height() - StepMarkerSize) / 2.0,
                                    StepMarkerSize,
                                    StepMarkerSize);

                // Connector down to the next step's marker
                if (i + 1 < m_titles.size()) {
                    painter.setPen(QPen(done ? colors.selected : colors.border, 2));
                    const qreal x = marker.center().x();
                    painter.drawLine(QPointF(x, marker.bottom() + 4), QPointF(x, rowRect(i + 1).top() + 9));
                }

                if (done) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(colors.selected);
                    painter.drawEllipse(marker);
                    painter.setPen(QPen(colors.onSelected, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    const QPointF c = marker.center();
                    painter.drawPolyline(QPolygonF({c + QPointF(-5, 0), c + QPointF(-1.5, 3.5), c + QPointF(5, -3.5)}));
                } else {
                    painter.setPen(QPen(current ? colors.selected : colors.border, current ? 2 : 1.5));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawEllipse(marker.adjusted(1, 1, -1, -1));
                    painter.setPen(current ? colors.selected : colors.textSecondary);
                    painter.setFont(VaultModal::pixelFont(font(), 13, QFont::DemiBold));
                    painter.drawText(marker, Qt::AlignCenter, QString::number(i + 1));
                }

                painter.setPen(done || current ? colors.text : colors.textSecondary);
                painter.setFont(VaultModal::pixelFont(font(), 15, current ? QFont::DemiBold : QFont::Normal));
                const QRect textRect = row.adjusted(StepMarkerLeft + StepMarkerSize + 12, 0, -8, 0);
                painter.drawText(textRect,
                                 Qt::AlignVCenter | Qt::AlignLeft,
                                 painter.fontMetrics().elidedText(m_titles[i], Qt::ElideRight, textRect.width()));
            }
        }

        void mouseMoveEvent(QMouseEvent* event) override
        {
            const int hovered = stepAt(event->pos());
            if (hovered != m_hovered) {
                m_hovered = hovered;
                setCursor(hovered >= 0 && hovered < m_current ? Qt::PointingHandCursor : Qt::ArrowCursor);
                update();
            }
        }

        void leaveEvent(QEvent* event) override
        {
            m_hovered = -1;
            update();
            QWidget::leaveEvent(event);
        }

        void mouseReleaseEvent(QMouseEvent* event) override
        {
            const int step = stepAt(event->pos());
            if (event->button() == Qt::LeftButton && step >= 0 && step < m_current && onStepClicked) {
                onStepClicked(step);
            }
        }

    private:
        QRect rowRect(int index) const
        {
            return {0, 4 + index * StepRowHeight, width(), StepRowHeight};
        }

        int stepAt(const QPoint& pos) const
        {
            for (int i = 0; i < m_titles.size(); ++i) {
                if (rowRect(i).contains(pos)) {
                    return i;
                }
            }
            return -1;
        }

        QStringList m_titles;
        int m_current = 0;
        int m_hovered = -1;
    };
} // namespace Vault

VaultNewDatabaseModal::VaultNewDatabaseModal(QWidget* window)
    : VaultModal(window, new QWidget(), tr("New Database"), QStringLiteral("vaultNewDatabase"))
{
    // Same pages and wording as the New Database wizard
    m_steps = {
        {tr("General"),
         tr("Please fill in the display name and an optional description for your new database:"),
         new DatabaseSettingWidgetMetaData()},
        {tr("Encryption"),
         tr("Here you can adjust the database encryption settings. "
            "Don't worry, you can change them later in the database settings."),
         new DatabaseSettingsWidgetEncryption()},
        {tr("Database Credentials"),
         tr("A set of credentials known only to you that protects your database."),
         new DatabaseSettingsWidgetDatabaseKey()},
    };

    QStringList titles;
    for (const auto& step : m_steps) {
        titles << step.title;
    }
    m_stepList = new Vault::NewDatabaseSteps(titles, card());
    m_stepList->setObjectName("vaultNewDatabaseSteps");
    m_stepList->onStepClicked = [this](int step) { showStep(step, false); };
    bodyLayout()->insertWidget(0, m_stepList);

    m_stepTitle = new QLabel(content());
    m_stepTitle->setObjectName("vaultNewDatabaseStepTitle");
    m_stepTitle->setFont(pixelFont(font(), 18, QFont::Bold));

    m_stepSubtitle = new QLabel(content());
    m_stepSubtitle->setObjectName("vaultNewDatabaseStepSubtitle");
    m_stepSubtitle->setFont(pixelFont(font(), 14));
    m_stepSubtitle->setWordWrap(true);
    // Room for two lines, so a long subtitle isn't squeezed by the page below it
    m_stepSubtitle->setMinimumHeight(QFontMetrics(m_stepSubtitle->font()).lineSpacing() * 2);
    m_stepSubtitle->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_stack = new QStackedWidget(content());
    m_stack->setObjectName("vaultNewDatabasePages");
    m_stack->setProperty(VaultLineEdit::LargePaddingProperty, true);
    for (const auto& step : asConst(m_steps)) {
        auto scroll = new QScrollArea(m_stack);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->viewport()->setAutoFillBackground(false);
        scroll->setWidget(step.page);
        step.page->setAutoFillBackground(false);
        m_stack->addWidget(scroll);
    }

    auto layout = new QVBoxLayout(content());
    layout->setContentsMargins(8, 4, 8, 0);
    layout->setSpacing(6);
    layout->addWidget(m_stepTitle);
    layout->addWidget(m_stepSubtitle);
    layout->addSpacing(10);
    layout->addWidget(m_stack, 1);

    auto cancelButton = addFooterButton(tr("Cancel"), false, QStringLiteral("Cancel"));
    connect(cancelButton, &QPushButton::clicked, this, &VaultNewDatabaseModal::reject);
    m_backButton = addFooterButton(tr("Back"), false, QStringLiteral("Back"));
    connect(m_backButton, &QPushButton::clicked, this, &VaultNewDatabaseModal::back);
    m_nextButton = addFooterButton(tr("Continue"), true, QStringLiteral("Continue"));
    connect(m_nextButton, &QPushButton::clicked, this, &VaultNewDatabaseModal::next);
    // Wide enough for either label, so the footer doesn't shift on the last step
    const auto metrics = m_nextButton->fontMetrics();
    m_nextButton->setMinimumWidth(
        qMax(metrics.horizontalAdvance(tr("Create Database")), metrics.horizontalAdvance(tr("Continue"))) + 48);

    setMaximumCardWidth(880);
    setInitialFocus(m_steps.first().page->findChild<QLineEdit*>("databaseName"));
}

VaultNewDatabaseModal::~VaultNewDatabaseModal() = default;

int VaultNewDatabaseModal::currentStep() const
{
    return m_current;
}

int VaultNewDatabaseModal::stepCount() const
{
    return m_steps.size();
}

void VaultNewDatabaseModal::next()
{
    // Like the wizard, a step is saved into the database before moving on
    auto page = m_steps[m_current].page;
    if (!page->saveSettings()) {
        return;
    }
    page->uninitialize();

    if (m_current + 1 < m_steps.size()) {
        showStep(m_current + 1, true);
        return;
    }

    auto db = m_db;
    m_db.reset();
    close();
    emit finished(db);
}

void VaultNewDatabaseModal::back()
{
    if (m_current > 0) {
        showStep(m_current - 1, false);
    }
}

void VaultNewDatabaseModal::accept()
{
    next();
}

void VaultNewDatabaseModal::reject()
{
    m_db.reset();
    close();
    emit finished({});
}

void VaultNewDatabaseModal::prepare()
{
    // Each opening starts over with an empty database, as the wizard does
    m_db = QSharedPointer<Database>::create();
    m_db->rootGroup()->setName(tr("Root", "Root group"));
    m_db->setKdf({});
    m_db->setKey({});
    showStep(0, true);
}

void VaultNewDatabaseModal::showStep(int index, bool load)
{
    m_current = index;
    const auto& step = m_steps[index];
    // Going forward loads the step from the database; going back keeps what was entered
    if (load) {
        step.page->loadSettings(m_db);
    }

    m_stack->setCurrentIndex(index);
    m_stepTitle->setText(step.title);
    m_stepSubtitle->setText(step.subtitle);
    m_stepList->setCurrent(index);
    m_backButton->setVisible(index > 0);
    const bool last = index + 1 == m_steps.size();
    m_nextButton->setText(last ? tr("Create Database") : tr("Continue"));

    // Pages that focus a field of their own, such as the name or password, keep it
    auto focused = window()->focusWidget();
    if (isOpen() && !(focused && m_stack->currentWidget()->isAncestorOf(focused))) {
        m_nextButton->setFocus(Qt::OtherFocusReason);
    }
}

void VaultNewDatabaseModal::applyColors()
{
    VaultModal::applyColors();

    // The base class gives every child the card palette, so color the headings after it
    const auto colors = VaultModal::colors();
    QPalette titlePalette = m_stepTitle->palette();
    titlePalette.setColor(QPalette::WindowText, colors.text);
    m_stepTitle->setPalette(titlePalette);
    QPalette subtitlePalette = m_stepSubtitle->palette();
    subtitlePalette.setColor(QPalette::WindowText, colors.textSecondary);
    m_stepSubtitle->setPalette(subtitlePalette);
    m_stepList->update();
}
