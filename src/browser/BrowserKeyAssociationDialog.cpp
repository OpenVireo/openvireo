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

#include "BrowserKeyAssociationDialog.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultWidgets.h"

#include <components/basicinput/Button.h>
#include <components/textfields/Label.h>

#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    // Width of the content dialog's card, as wide as the other browser requests
    constexpr int VaultDialogWidth = 480;

    fluent::textfields::Label*
    vaultLabel(QWidget* parent, Typography::FontRole font, fluent::textfields::Label::TextColorRole color)
    {
        auto label = new fluent::textfields::Label(parent);
        label->setFluentTypography(font);
        label->setTextColorRole(color);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        return label;
    }
} // namespace

// Helper text under the name field, drawn in the caution color while it warns. Fluent-Qt's labels only know neutral
// and accent colors, and reapply theirs on every theme change, so the warning carries its color in its own markup.
class BrowserKeyAssociationHint : public fluent::textfields::Label
{
public:
    explicit BrowserKeyAssociationHint(QWidget* parent)
        : Label(parent)
    {
        setFluentTypography(Typography::FontRole::Caption);
        setTextColorRole(TextColorRole::Secondary);
        setTextFormat(Qt::RichText);
        setWordWrap(true);
    }

    void setHint(const QString& text, bool caution)
    {
        m_text = text;
        m_caution = caution;
        render();
    }

    bool isCaution() const
    {
        return m_caution;
    }

    void onThemeUpdated() override
    {
        Label::onThemeUpdated();
        render();
    }

private:
    void render()
    {
        const auto escaped = m_text.toHtmlEscaped();
        setText(m_caution ? QStringLiteral("<span style=\"color:%1\">%2</span>")
                                .arg(FluentTheme::palette().caution.name(), escaped)
                          : escaped);
    }

    QString m_text;
    bool m_caution = false;
};

BrowserKeyAssociationDialog::BrowserKeyAssociationDialog(const QString& databaseName,
                                                         std::function<bool(const QString&)> nameInUse,
                                                         QWidget* window)
    : ContentDialog(window)
    , m_nameInUse(std::move(nameInUse))
    , m_saveText(tr("Save and allow access"))
    , m_replaceText(tr("Replace and allow access"))
    , m_defaultHint(tr("For example, chrome-laptop. You can see and remove connections in Browser Integration "
                       "settings."))
{
    using fluent::textfields::Label;
    setObjectName("keyAssociationDialog");
    setThemeSource(window);
    setTitle(tr("Connect a browser?"));
    setPrimaryButtonText(m_saveText);
    setCloseButtonText(tr("Cancel"));
    setDefaultButton(ContentDialog::Primary);

    auto content = new QWidget(this);
    auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto message = vaultLabel(content, Typography::FontRole::Body, Label::TextColorRole::Primary);
    message->setObjectName("keyAssociationMessage");
    message->setText(tr("A browser extension is asking to connect to the database “%1”. "
                        "Give this connection a name so you can recognise it later.")
                         .arg(databaseName));
    layout->addWidget(message);
    layout->addSpacing(16);

    // A WinUI text box: header above the field, helper text below it
    auto header = vaultLabel(content, Typography::FontRole::Body, Label::TextColorRole::Primary);
    header->setText(tr("Connection name"));
    layout->addWidget(header);
    layout->addSpacing(8);

    m_nameEdit = new VaultLineEdit(content);
    m_nameEdit->setObjectName("keyAssociationName");
    m_nameEdit->setPlaceholderText(tr("chrome-laptop"));
    m_nameEdit->setClearButtonEnabled(true);
    header->setBuddy(m_nameEdit);
    layout->addWidget(m_nameEdit);
    layout->addSpacing(6);

    m_nameHint = new BrowserKeyAssociationHint(content);
    m_nameHint->setObjectName("keyAssociationHint");
    layout->addWidget(m_nameHint);

    // The text wraps, so the content asks for its height at the width the dialog gives it
    setContent(new VaultFittedContent(content));
    resize(VaultDialogWidth + 2 * shadowSize(), minimumHeight());

    for (auto button : findChildren<fluent::basicinput::Button*>()) {
        if (button->text() == m_saveText) {
            m_saveButton = button;
        }
    }
    Q_ASSERT(m_saveButton);
    // Enter in the field presses it, while there is a name to save
    if (auto button = qobject_cast<QPushButton*>(m_saveButton)) {
        button->setDefault(true);
    }

    connect(m_nameEdit, &QLineEdit::textChanged, this, &BrowserKeyAssociationDialog::updateName);
    updateName();
}

bool BrowserKeyAssociationDialog::canShowOver(QWidget* widget)
{
    return FluentTheme::vaultLayoutEnabled() && widget && widget->window()->isVisible()
           && !widget->window()->isMinimized();
}

QString BrowserKeyAssociationDialog::name() const
{
    return m_nameEdit->text().trimmed();
}

bool BrowserKeyAssociationDialog::replacesExisting() const
{
    const auto id = name();
    return !id.isEmpty() && m_nameInUse && m_nameInUse(id);
}

void BrowserKeyAssociationDialog::updateName()
{
    const bool replaces = replacesExisting();
    m_saveButton->setEnabled(!name().isEmpty());
    // The button keeps its place; only its label says what saving does
    m_saveButton->setText(replaces ? m_replaceText : m_saveText);
    m_nameHint->setHint(replaces ? tr("A connection named “%1” already exists. Saving replaces its key, "
                                      "and the browser that used it will have to connect again.")
                                       .arg(name())
                                 : m_defaultHint,
                        replaces);
}

bool BrowserKeyAssociationDialog::hintIsCaution() const
{
    return m_nameHint->isCaution();
}

void BrowserKeyAssociationDialog::showEvent(QShowEvent* event)
{
    ContentDialog::showEvent(event);
    // The dialog takes focus once it is shown, so the field takes it back afterwards
    QTimer::singleShot(0, m_nameEdit, [edit = m_nameEdit] { edit->setFocus(Qt::OtherFocusReason); });
}
