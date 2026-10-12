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

#include "VaultSettingsPage.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultWidgets.h"

#include <components/basicinput/ToggleSwitch.h>

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDebug>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QTextEdit>

#include <algorithm>

namespace
{
    constexpr int CardRadius = 4;
    constexpr int CardPadding = 16;
    constexpr int CardMinimumHeight = 48;
    constexpr int CardSpacing = 4;
    constexpr int ChildIndent = 40;
    constexpr int ChevronSize = 32;
    constexpr int HeaderSpacing = 12;
    constexpr int SectionGap = 20;
    constexpr int MinimumFieldWidth = 120;

    // Hidden by the page's own code, as opposed to hidden along with its parent
    bool explicitlyHidden(const QWidget* widget)
    {
        return widget->testAttribute(Qt::WA_WState_Hidden) && widget->testAttribute(Qt::WA_WState_ExplicitShowHide);
    }

    // Move a widget to a new parent, keeping whether the page hid it
    void adopt(QWidget* widget, QWidget* parent)
    {
        const bool hidden = explicitlyHidden(widget);
        widget->setParent(parent);
        widget->setVisible(!hidden);
    }

    // Caption of a form label without its trailing colon, as Fluent settings show it
    QString captionText(const QString& text)
    {
        QString caption = text.trimmed();
        while (caption.endsWith(QLatin1Char(':')) || caption.endsWith(QChar(0xff1a))) {
            caption.chop(1);
        }
        return caption.trimmed();
    }

    /**
     * Keeps a label in a Fluent font and text color. The settings modal resets the palettes of its
     * content when it opens, so the color is put back whenever the palette changes.
     */
    class LabelStyle : public QObject
    {
    public:
        LabelStyle(QLabel* label, Typography::FontRole role, bool secondary)
            : QObject(label)
            , m_label(label)
            , m_role(role)
            , m_secondary(secondary)
        {
            label->installEventFilter(this);
            Vault::followTheme(this, [this] { apply(); });
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == m_label && event->type() == QEvent::PaletteChange) {
                apply();
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        void apply()
        {
            const auto p = FluentTheme::palette();
            const QColor color = m_secondary ? p.textSecondary : p.text;
            const QFont font = Vault::font(m_role);
            if (m_label->font() != font) {
                m_label->setFont(font);
            }
            QPalette palette = m_label->palette();
            if (palette.color(QPalette::Active, QPalette::WindowText) == color
                && palette.color(QPalette::Disabled, QPalette::WindowText) == p.textDisabled) {
                return;
            }
            for (auto group : {QPalette::Active, QPalette::Inactive}) {
                palette.setColor(group, QPalette::WindowText, color);
                palette.setColor(group, QPalette::Text, color);
            }
            palette.setColor(QPalette::Disabled, QPalette::WindowText, p.textDisabled);
            palette.setColor(QPalette::Disabled, QPalette::Text, p.textDisabled);
            m_label->setPalette(palette);
        }

        QLabel* m_label;
        Typography::FontRole m_role;
        bool m_secondary;
    };

    void styleLabel(QLabel* label, Typography::FontRole role, bool secondary = false)
    {
        new LabelStyle(label, role, secondary);
    }

    // A label's enabled state follows a control's, so a disabled setting reads as disabled
    class EnabledFollower : public QObject
    {
    public:
        EnabledFollower(QWidget* source, QWidget* target)
            : QObject(target)
            , m_target(target)
        {
            source->installEventFilter(this);
            target->setEnabled(source->isEnabled());
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::EnabledChange) {
                m_target->setEnabled(static_cast<QWidget*>(watched)->isEnabled());
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        QWidget* m_target;
    };

    // Toggle switch that mirrors a check box, which keeps the state for the page's code
    fluent::basicinput::ToggleSwitch* switchFor(QAbstractButton* checkBox, QWidget* parent)
    {
        auto toggle = new fluent::basicinput::ToggleSwitch(parent);
        toggle->setObjectName(checkBox->objectName() + QStringLiteral("Switch"));
        // The check box keeps the focus, so Tab and Space work on it as before
        toggle->setFocusPolicy(Qt::NoFocus);
        toggle->setAccessibleName(VaultButton::withoutMnemonic(checkBox->text()));
        toggle->setToolTip(checkBox->toolTip());
        toggle->setIsOn(checkBox->isChecked());
        toggle->setEnabled(checkBox->isEnabled());
        toggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        QObject::connect(checkBox, &QAbstractButton::toggled, toggle, [toggle](bool checked) {
            if (toggle->isOn() != checked) {
                toggle->setIsOn(checked);
            }
        });
        QObject::connect(toggle, &fluent::basicinput::ToggleSwitch::toggled, checkBox, [checkBox](bool on) {
            if (checkBox->isChecked() != on) {
                checkBox->setChecked(on);
            }
        });
        new EnabledFollower(checkBox, toggle);
        return toggle;
    }
} // namespace

VaultSettingsPage::VaultSettingsPage(QWidget* page, const QString& title)
    : m_page(page)
    , m_body(new QWidget(page))
    , m_layout(new QVBoxLayout(m_body))
{
    m_body->setObjectName(page->objectName() + QStringLiteral("Cards"));
    m_layout->setContentsMargins(4, 4, 12, 16);
    m_layout->setSpacing(0);

    auto heading = new QLabel(title, m_body);
    heading->setObjectName(QStringLiteral("vaultSettingsPageTitle"));
    heading->setAccessibleName(title);
    styleLabel(heading, Typography::FontRole::Subtitle);
    m_layout->addWidget(heading);
}

VaultSettingsSection* VaultSettingsPage::addSection(const QString& title)
{
    auto section = new VaultSettingsSection(title, m_body);
    m_layout->addWidget(section);
    return section;
}

void VaultSettingsPage::finish()
{
    if (!m_page) {
        return;
    }
    m_layout->addStretch();

    // Keep what's left of the form, such as its labels and layouts that the page's code may still use, out of sight
    auto form = new QWidget(m_page);
    form->setObjectName(QStringLiteral("vaultSettingsPageForm"));
    if (auto layout = m_page->layout()) {
        form->setLayout(layout);
    }
    form->hide();

    auto layout = new QVBoxLayout(m_page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_body);

    // Controls for spin boxes and text areas, which have no Fluent-Qt counterpart
    Vault::followTheme(m_body, [body = m_body] { body->setStyleSheet(Vault::formStyleSheet()); });

    for (auto widget : form->findChildren<QWidget*>()) {
        const bool input = qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget)
                           || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QAbstractItemView*>(widget)
                           || qobject_cast<QTextEdit*>(widget) || qobject_cast<QPlainTextEdit*>(widget)
                           || (qobject_cast<QLineEdit*>(widget) && !qobject_cast<QComboBox*>(widget->parentWidget())
                               && !qobject_cast<QAbstractSpinBox*>(widget->parentWidget()));
        if (!input) {
            continue;
        }
        bool hidden = false;
        for (auto w = widget; w && w != form; w = w->parentWidget()) {
            hidden = hidden || explicitlyHidden(w);
        }
        if (!hidden) {
            qWarning("VaultSettingsPage: %s is missing from the settings cards of %s",
                     qPrintable(widget->objectName()),
                     qPrintable(m_page->objectName()));
        }
    }
}

VaultSettingsSection::VaultSettingsSection(const QString& title, QWidget* parent)
    : QWidget(parent)
    , m_heading(new QLabel(title, this))
    , m_cards(new QVBoxLayout())
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, SectionGap, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(m_heading);
    layout->addLayout(m_cards);
    m_cards->setContentsMargins(0, 0, 0, 0);
    m_cards->setSpacing(CardSpacing);

    m_heading->setVisible(!title.isEmpty());
    styleLabel(m_heading, Typography::FontRole::BodyStrong);
}

VaultSettingsCard* VaultSettingsSection::addToggle(QAbstractButton* checkBox, const QString& description)
{
    auto card = new VaultSettingsCard(this);
    card->setToggle(checkBox, description);
    return addCard(card);
}

VaultSettingsCard*
VaultSettingsSection::addSetting(const VaultSettingsCaption& caption, QWidget* control, const QString& description)
{
    return addSetting(caption, QWidgetList{control}, description);
}

VaultSettingsCard* VaultSettingsSection::addSetting(const VaultSettingsCaption& caption,
                                                    const QWidgetList& controls,
                                                    const QString& description)
{
    auto card = new VaultSettingsCard(this);
    card->setSetting(caption, controls, description);
    return addCard(card);
}

VaultSettingsCard* VaultSettingsSection::addWidget(QWidget* widget)
{
    auto card = new VaultSettingsCard(this);
    card->setWidget(widget);
    return addCard(card);
}

VaultSettingsCard* VaultSettingsSection::addCard(VaultSettingsCard* card)
{
    m_cards->addWidget(card);
    connect(card, &VaultSettingsCard::visibilityChanged, this, &VaultSettingsSection::updateVisibility);
    updateVisibility();
    return card;
}

void VaultSettingsSection::updateVisibility()
{
    // A section whose settings are all hidden, such as ones that don't apply on this platform, goes with them
    bool visible = false;
    for (int i = 0; i < m_cards->count() && !visible; ++i) {
        auto card = m_cards->itemAt(i)->widget();
        visible = card && !explicitlyHidden(card);
    }
    setVisible(visible);
}

/**
 * A row of a settings card: a caption or check box on the left and the controls on the right.
 * When the row is too narrow for both, the controls move below the caption, as in Windows Settings.
 */
class VaultSettingsRow : public QWidget
{
public:
    explicit VaultSettingsRow(QWidget* parent)
        : QWidget(parent)
        , m_grid(new QGridLayout(this))
        , m_controls(new QWidget(this))
        , m_controlLayout(new QHBoxLayout(m_controls))
    {
        setMinimumHeight(CardMinimumHeight);
        m_grid->setContentsMargins(CardPadding, 8, CardPadding, 8);
        m_grid->setHorizontalSpacing(HeaderSpacing);
        m_grid->setVerticalSpacing(8);
        m_grid->setColumnStretch(0, 1);
        m_controlLayout->setContentsMargins(0, 0, 0, 0);
        m_controlLayout->setSpacing(8);
        m_grid->addWidget(m_controls, 0, 1, Qt::AlignVCenter);
        m_controls->hide();
    }

    void setLead(QWidget* lead)
    {
        m_lead = lead;
        m_grid->addWidget(lead, 0, 0, Qt::AlignVCenter);
    }

    void setLead(QLayout* lead, QWidget* minimumWidget)
    {
        m_leadLayout = lead;
        m_lead = minimumWidget;
        m_grid->addLayout(lead, 0, 0, Qt::AlignVCenter);
    }

    // Lay a widget across the full width, such as a table
    void setWide(QWidget* widget)
    {
        m_wide = true;
        m_lead = widget;
        m_grid->setContentsMargins(CardPadding, CardPadding - 4, CardPadding, CardPadding - 4);
        m_grid->addWidget(widget, 0, 0);
    }

    void insertControl(int index, QWidget* control)
    {
        adopt(control, m_controls);
        // Fields keep their natural width where there's room, but don't hold the page wider than the modal
        if (qobject_cast<QComboBox*>(control) || qobject_cast<QLineEdit*>(control)) {
            control->setMinimumWidth(MinimumFieldWidth);
        }
        // Text fields share the row with the caption instead of keeping their short natural width
        if (qobject_cast<QLineEdit*>(control)) {
            control->setSizePolicy(QSizePolicy::Expanding, control->sizePolicy().verticalPolicy());
            m_grid->setColumnStretch(0, 0);
            m_grid->setColumnStretch(1, 1);
        }
        m_controlLayout->insertWidget(index < 0 ? m_controlLayout->count() : index, control, 0, Qt::AlignVCenter);
        m_controls->show();
    }

    int controlCount() const
    {
        return m_controlLayout->count();
    }

    void setTrailing(QWidget* trailing)
    {
        m_trailing = trailing;
        m_grid->addWidget(trailing, 0, 2, Qt::AlignVCenter);
    }

    void setIndent(int left, int right)
    {
        const auto margins = m_grid->contentsMargins();
        m_grid->setContentsMargins(left, margins.top(), right, margins.bottom());
    }

    QSize minimumSizeHint() const override
    {
        // Narrow enough for the controls below the caption
        const auto margins = m_grid->contentsMargins();
        int width = m_lead ? qMin(m_lead->minimumSizeHint().width(), MinimumCaptionWidth) : 0;
        if (m_controls->isVisibleTo(this)) {
            width = qMax(width, m_controls->minimumSizeHint().width());
        }
        if (m_trailing && m_trailing->isVisibleTo(this)) {
            width += HeaderSpacing + m_trailing->sizeHint().width();
        }
        return {width + margins.left() + margins.right(), CardMinimumHeight};
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        arrange(event->size().width());
        QWidget::resizeEvent(event);
    }

private:
    void arrange(int width)
    {
        if (m_wide || !m_controls->isVisibleTo(this)) {
            return;
        }
        const auto margins = m_grid->contentsMargins();
        int available = width - margins.left() - margins.right() - HeaderSpacing;
        if (m_trailing && m_trailing->isVisibleTo(this)) {
            available -= HeaderSpacing + m_trailing->sizeHint().width();
        }
        const int captionWidth = m_lead ? qMin(m_lead->sizeHint().width(), MinimumCaptionWidth) : 0;
        const bool stacked = available < captionWidth + m_controls->sizeHint().width();
        if (stacked == m_stacked) {
            return;
        }
        m_stacked = stacked;
        m_grid->removeWidget(m_controls);
        if (stacked) {
            m_grid->addWidget(m_controls, 1, 0, Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            m_grid->addWidget(m_controls, 0, 1, Qt::AlignVCenter);
        }
    }

    static constexpr int MinimumCaptionWidth = 200;

    QGridLayout* m_grid;
    QWidget* m_controls;
    QHBoxLayout* m_controlLayout;
    QPointer<QWidget> m_lead;
    QLayout* m_leadLayout = nullptr;
    QPointer<QWidget> m_trailing;
    bool m_wide = false;
    bool m_stacked = false;
};

VaultSettingsCard::VaultSettingsCard(QWidget* parent)
    : QWidget(parent)
    , m_header(new VaultSettingsRow(this))
    , m_children(new QWidget(this))
    , m_childLayout(new QVBoxLayout(m_children))
{
    m_childLayout->setContentsMargins(0, 0, 0, 0);
    m_childLayout->setSpacing(0);
    m_children->hide();

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_header);
    layout->addWidget(m_children);

    Vault::followTheme(this, [this] { update(); });
}

void VaultSettingsCard::setToggle(QAbstractButton* checkBox, const QString& description)
{
    m_control = checkBox;
    m_toggleHeader = true;
    fillToggleRow(m_header, checkBox, description);
    watch(checkBox);
    updateRows();
}

void VaultSettingsCard::setSetting(const VaultSettingsCaption& caption,
                                   const QWidgetList& controls,
                                   const QString& description)
{
    m_control = controls.value(0);
    fillSettingRow(m_header, caption, controls, description);
    watch(m_control);
    updateRows();
}

void VaultSettingsCard::setWidget(QWidget* widget)
{
    m_control = widget;
    adopt(widget, m_header);
    m_header->setWide(widget);
    watch(widget);
    updateRows();
}

void VaultSettingsCard::addHeaderControl(QWidget* control)
{
    // Before the switch of a toggle, or before the main control of a setting
    m_header->insertControl(m_toggleHeader ? m_header->controlCount() - 1 : 0, control);
}

void VaultSettingsCard::addToggle(QAbstractButton* checkBox, const QString& description)
{
    auto row = addRow();
    fillToggleRow(row, checkBox, description);
    m_rows.append(Row{row, checkBox});
    watch(checkBox);
    updateRows();
}

void VaultSettingsCard::addSetting(const VaultSettingsCaption& caption, QWidget* control, const QString& description)
{
    addSetting(caption, QWidgetList{control}, description);
}

void VaultSettingsCard::addSetting(const VaultSettingsCaption& caption,
                                   const QWidgetList& controls,
                                   const QString& description)
{
    auto row = addRow();
    fillSettingRow(row, caption, controls, description);
    m_rows.append(Row{row, controls.value(0)});
    watch(controls.value(0));
    updateRows();
}

void VaultSettingsCard::addWidget(QWidget* widget)
{
    auto row = addRow();
    adopt(widget, row);
    row->setLead(widget);
    m_rows.append(Row{row, widget});
    watch(widget);
    updateRows();
}

void VaultSettingsCard::fillToggleRow(VaultSettingsRow* row, QAbstractButton* checkBox, const QString& description)
{
    adopt(checkBox, row);
    if (auto vaultCheckBox = qobject_cast<VaultCheckBox*>(checkBox)) {
        vaultCheckBox->setSwitchStyle(true);
        vaultCheckBox->setDescription(description);
    }
    row->setLead(checkBox);
    row->insertControl(-1, switchFor(checkBox, row));

    // Draw Fluent's focus ring around the row while the check box has keyboard focus
    checkBox->installEventFilter(this);
    checkBox->setProperty("vaultSettingsRow", QVariant::fromValue(static_cast<QObject*>(row)));
}

void VaultSettingsCard::fillSettingRow(VaultSettingsRow* row,
                                       const VaultSettingsCaption& caption,
                                       const QWidgetList& controls,
                                       const QString& description)
{
    auto control = controls.value(0);
    auto label = caption.label;
    if (label) {
        adopt(label, row);
        label->setText(captionText(label->text()));
        // Form labels are often right-aligned against their field; a card's caption reads from the left
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    } else {
        label = new QLabel(caption.title, row);
    }
    if (!label->buddy()) {
        label->setBuddy(control);
    }
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    styleLabel(label, Typography::FontRole::Body);
    new EnabledFollower(control, label);

    auto texts = new QVBoxLayout();
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(2);
    texts->addWidget(label);
    if (!description.isEmpty()) {
        auto descriptionLabel = new QLabel(description, row);
        descriptionLabel->setWordWrap(true);
        styleLabel(descriptionLabel, Typography::FontRole::Caption, true);
        new EnabledFollower(control, descriptionLabel);
        texts->addWidget(descriptionLabel);
    }
    row->setLead(texts, label);

    for (auto widget : controls) {
        row->insertControl(-1, widget);
    }
}

VaultSettingsRow* VaultSettingsCard::addRow()
{
    if (!m_chevron) {
        m_chevron = Vault::iconButton(Typography::Icons::ChevronUp, tr("Hide options"), m_header);
        m_chevron->setObjectName(QStringLiteral("vaultSettingsCardChevron"));
        m_chevron->setFixedSize(ChevronSize, ChevronSize);
        m_header->setTrailing(m_chevron);
        connect(m_chevron, &QAbstractButton::clicked, this, [this] { setExpanded(!m_expanded); });
    }

    // Child rows line up their controls with the header's, which has the chevron after it
    auto row = new VaultSettingsRow(m_children);
    row->setIndent(ChildIndent, CardPadding + ChevronSize + HeaderSpacing);
    m_childLayout->addWidget(row);
    return row;
}

bool VaultSettingsCard::isExpanded() const
{
    return m_expanded;
}

void VaultSettingsCard::setExpanded(bool expanded)
{
    if (m_expanded == expanded) {
        return;
    }
    m_expanded = expanded;
    if (m_chevron) {
        m_chevron->setIconGlyph(expanded ? Typography::Icons::ChevronUp : Typography::Icons::ChevronDown);
        const auto toolTip = expanded ? tr("Hide options") : tr("Show options");
        m_chevron->setToolTip(toolTip);
        m_chevron->setAccessibleName(toolTip);
    }
    updateRows();
}

void VaultSettingsCard::watch(QWidget* control)
{
    if (control) {
        control->installEventFilter(this);
    }
}

void VaultSettingsCard::updateRows()
{
    bool anyChild = false;
    for (const auto& row : m_rows) {
        const bool visible = row.control && !explicitlyHidden(row.control);
        row.row->setVisible(visible);
        anyChild = anyChild || visible;
    }
    m_children->setVisible(anyChild && m_expanded);
    if (m_chevron) {
        m_chevron->setVisible(anyChild);
    }

    const bool visible = !m_control || !explicitlyHidden(m_control);
    if (explicitlyHidden(this) == visible) {
        setVisible(visible);
        emit visibilityChanged();
    }
    update();
}

bool VaultSettingsCard::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
        if (watched == m_control
            || std::any_of(m_rows.cbegin(), m_rows.cend(), [watched](const Row& r) { return r.control == watched; })) {
            updateRows();
        }
        break;
    case QEvent::FocusIn: {
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        m_focusRow = qobject_cast<QWidget*>(watched->property("vaultSettingsRow").value<QObject*>());
        m_keyboardFocus =
            reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
        update();
        break;
    }
    case QEvent::FocusOut:
        m_focusRow = nullptr;
        m_keyboardFocus = false;
        update();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void VaultSettingsCard::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(p.stroke, 1));
    painter.setBrush(p.inset);
    painter.drawRoundedRect(card, CardRadius, CardRadius);

    // Dividers between the header and each row below it
    if (m_children->isVisible()) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(p.divider, 1));
        for (const auto& row : m_rows) {
            if (row.row->isVisible()) {
                const int y = row.row->mapTo(this, QPoint(0, 0)).y();
                painter.drawLine(1, y, width() - 2, y);
            }
        }
        painter.setRenderHint(QPainter::Antialiasing);
    }

    if (m_focusRow && m_keyboardFocus) {
        QRectF ring = QRectF(m_focusRow == m_header ? QRect(m_header->pos(), m_header->size())
                                                    : QRect(m_focusRow->mapTo(this, QPoint(0, 0)), m_focusRow->size()));
        if (m_focusRow == m_header && !m_children->isVisible()) {
            ring = rect();
        }
        painter.setPen(QPen(p.text, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(ring.adjusted(1, 1, -1, -1), CardRadius, CardRadius);
    }
}
