#!/usr/bin/env bash
#
# Fails if a Qt Designer form (.ui) uses a stock control that has a Fluent drop-in in
# src/gui/vault/VaultControls.h. There are no exceptions.
#
# Usage: check-fluent-ui.sh [source-dir]

set -eu

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="${1:-$(cd "${script_dir}/../.." && pwd)}"

stock_classes='QPushButton|QCheckBox|QRadioButton|QComboBox|QLineEdit|QSpinBox|QGroupBox|QDialogButtonBox'

# Stock class -> its Fluent drop-in
replacement() {
    case "$1" in
        QPushButton) echo "VaultButton" ;;
        QCheckBox) echo "VaultCheckBox (or a settings-card toggle)" ;;
        QRadioButton) echo "VaultRadioButton" ;;
        QComboBox) echo "VaultComboBox" ;;
        QLineEdit) echo "VaultLineEdit" ;;
        QSpinBox) echo "VaultSpinBox" ;;
        QGroupBox) echo "VaultCard (or a settings-page section)" ;;
        QDialogButtonBox) echo "VaultDialogButtonBox" ;;
    esac
}

cd "${source_dir}"
status=0
forms="$(find src -name '*.ui' | sed 's|^\./||' | sort)"
for form in ${forms}; do
    stock="$(grep -oE "<widget class=\"(${stock_classes})\" name=\"[^\"]*\"" "${form}" || true)"
    if [ -n "${stock}" ]; then
        printf '%s\n' "${stock}" | while IFS= read -r match; do
            class="$(printf '%s' "${match}" | sed -E 's/<widget class="([A-Za-z]+)".*/\1/')"
            name="$(printf '%s' "${match}" | sed -E 's/.*name="([^"]*)"/\1/')"
            echo "${form}: ${name} is a stock ${class}; use $(replacement "${class}") from gui/vault/VaultControls.h"
        done
        status=1
    fi
done

if [ ${status} -eq 0 ]; then
    echo "All forms use Fluent controls."
fi
exit ${status}
