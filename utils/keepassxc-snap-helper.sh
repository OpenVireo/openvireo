#!/usr/bin/env bash
#
# OpenVireo Browser Extension Native Messaging Installer Tool
# Copyright (C) 2017 KeePassXC team <https://keepassxc.org/>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 or (at your option)
# version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

set -e

JSON_OUT=""
BASE_DIR="."
INSTALL_DIR=""
INSTALL_FILE="com.openvireo.browser.json"

# Early out if the openvireo.proxy executable cannot be found
if ! command -v openvireo.proxy; then
    echo "Could not find openvireo.proxy! Ensure the openvireo snap is installed properly."
    exit 0
fi

PROXY_PATH=$(command -v openvireo.proxy)

JSON_FIREFOX=$(cat << EOF
{
    "name": "com.openvireo.browser",
    "description": "OpenVireo integration with native messaging support",
    "path": "${PROXY_PATH}",
    "type": "stdio",
    "allowed_extensions": [
        "browser@openvireo.com"
    ]
}
EOF
)

JSON_CHROME=$(cat << EOF
{
    "name": "com.openvireo.browser",
    "description": "OpenVireo integration with native messaging support",
    "path": "${PROXY_PATH}",
    "type": "stdio",
    "allowed_origins": [
        "chrome-extension://bafnojkbdaiajjkkaeljcngflakgdmmk/"
    ]
}
EOF
)

setupFirefox() {
    JSON_OUT=${JSON_FIREFOX}
    INSTALL_DIR="${BASE_DIR}/.mozilla/native-messaging-hosts"
}

setupChrome() {
    JSON_OUT=${JSON_CHROME}
    INSTALL_DIR="${BASE_DIR}/.config/google-chrome/NativeMessagingHosts"
}

setupChromium() {
    JSON_OUT=${JSON_CHROME}
    INSTALL_DIR="${BASE_DIR}/.config/chromium/NativeMessagingHosts"
}

setupVivaldi() {
    JSON_OUT=${JSON_CHROME}
    INSTALL_DIR="${BASE_DIR}/.config/vivaldi/NativeMessagingHosts"
}

setupBrave() {
    JSON_OUT=${JSON_CHROME}
    INSTALL_DIR="${BASE_DIR}/.config/BraveSoftware/Brave-Browser/NativeMessagingHosts"
}

setupTorBrowser() {
    JSON_OUT=${JSON_FIREFOX}
    INSTALL_DIR="${BASE_DIR}/.tor-browser/app/Browser/TorBrowser/Data/Browser/.mozilla/native-messaging-hosts"
}

setupEdge() {
    JSON_OUT=${JSON_CHROME}
    INSTALL_DIR="${BASE_DIR}/.config/microsoft-edge/NativeMessagingHosts"
}

setupLibreWolf() {
    JSON_OUT=${JSON_FIREFOX}
    INSTALL_DIR="${BASE_DIR}/.librewolf/native-messaging-hosts"
}

# --------------------------------
# Start of script
# --------------------------------

BROWSER=$(whiptail \
            --title "Browser Selection" \
            --menu "Choose a browser to integrate with OpenVireo:" \
            15 60 5 \
            "1" "Firefox" \
            "2" "Chrome" \
            "3" "Chromium" \
            "4" "Vivaldi" \
            "5" "Brave" \
            "6" "Tor Browser" \
            "7" "Microsoft Edge" \
            "8" "LibreWolf" \
            3>&1 1>&2 2>&3)

exitstatus=$?

clear

if [[ $exitstatus == 0 ]]; then
    # Configure settings for the chosen browser
    case $BROWSER in
        1) setupFirefox ;;
        2) setupChrome ;;
        3) setupChromium ;;
        4) setupVivaldi ;;
        5) setupBrave ;;
        6) setupTorBrowser ;;
        7) setupEdge ;;
        8) setupLibreWolf ;;
    esac

    # Install the JSON file
    cd ~
    mkdir -p "$INSTALL_DIR"
    echo "$JSON_OUT" > "${INSTALL_DIR}/${INSTALL_FILE}"

    whiptail \
        --title "Installation Complete" \
        --msgbox "You will need to restart your browser in order to connect to OpenVireo" \
        8 50
else
    whiptail --title "Installation Canceled" --msgbox "No changes were made to your system" 8 50
fi
