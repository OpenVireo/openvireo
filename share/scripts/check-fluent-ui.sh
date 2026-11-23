#!/usr/bin/env bash
#
# Fails if a Qt Designer form (.ui) uses a stock control that has a Fluent drop-in in
# src/gui/vault/VaultControls.h, unless the form is listed in fluent-ui-allowlist.txt.
# Also fails for allowlisted forms that no longer need to be there, so the list only shrinks.
#
# Usage: check-fluent-ui.sh [source-dir]

set -eu

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="${1:-$(cd "${script_dir}/../.." && pwd)}"
allowlist="${script_dir}/fluent-ui-allowlist.txt"

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

# Forms of the allowlist, one per line, without comments and blank lines
allowed=""
if [ -f "${allowlist}" ]; then
    allowed="$(sed -e 's/#.*//' -e 's/[[:space:]]//g' "${allowlist}" | grep -v '^$' || true)"
fi
is_allowed() {
    [ -n "${allowed}" ] && printf '%s\n' "${allowed}" | grep -qxF "$1"
}

cd "${source_dir}"
status=0
count=0
forms="$(find src -name '*.ui' | sed 's|^\./||' | sort)"
for form in ${forms}; do
    stock="$(grep -oE "<widget class=\"(${stock_classes})\" name=\"[^\"]*\"" "${form}" || true)"
    if is_allowed "${form}"; then
        count=$((count + 1))
        if [ -z "${stock}" ]; then
            echo "${form}: no stock controls left; remove it from share/scripts/fluent-ui-allowlist.txt"
            status=1
        fi
        continue
    fi
    if [ -n "${stock}" ]; then
        printf '%s\n' "${stock}" | while IFS= read -r match; do
            class="$(printf '%s' "${match}" | sed -E 's/<widget class="([A-Za-z]+)".*/\1/')"
            name="$(printf '%s' "${match}" | sed -E 's/.*name="([^"]*)"/\1/')"
            echo "${form}: ${name} is a stock ${class}; use $(replacement "${class}") from gui/vault/VaultControls.h"
        done
        status=1
    fi
done

if [ -n "${allowed}" ]; then
    for form in ${allowed}; do
        if [ ! -f "${form}" ]; then
            echo "${form}: listed in share/scripts/fluent-ui-allowlist.txt but not found"
            status=1
        fi
    done
fi

if [ ${status} -eq 0 ]; then
    echo "All forms use Fluent controls (${count} allowlisted until their redesign)."
fi
exit ${status}
