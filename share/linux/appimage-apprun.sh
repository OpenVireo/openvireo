#!/usr/bin/env bash

export PATH="$(dirname $0)/usr/bin:${PATH}"

if [ "$1" == "cli" ] || [ "$(basename "$ARGV0")" == "openvireo-cli" ] || [ "$(basename "$ARGV0")" == "openvireo-cli.AppImage" ]; then
    [ "$1" == "cli" ] && shift
    exec openvireo-cli "$@"
elif [ "$1" == "proxy" ] || [ "$(basename "$ARGV0")" == "openvireo-proxy" ] || [ "$(basename "$ARGV0")" == "openvireo-proxy.AppImage" ]; then
    [ "$1" == "proxy" ] && shift
    exec openvireo-proxy "$@"
elif [ -v CHROME_WRAPPER ] || [ -v MOZ_LAUNCHED_CHILD ] || [ "$2" == "browser@openvireo.com" ]; then
    exec openvireo-proxy "$@"
else
    # --- Icon file setup ---
    icon_source="$APPDIR/usr/share/icons/hicolor/256x256/apps/openvireo.png"
    icon_target="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps/openvireo.png"
    mkdir -p "$(dirname "$icon_target")"

    # Copy icon if different or missing
    if [ ! -f "$icon_target" ] || ! cmp -s "$icon_source" "$icon_target"; then
        echo "Installing OpenVireo icon to ${icon_target}"
        cp "$icon_source" "$icon_target"
    fi

    # --- Desktop file setup ---
    desktop_source="$APPDIR/usr/share/applications/com.openvireo.OpenVireo.desktop"
    desktop_target="${XDG_DATA_HOME:-$HOME/.local/share}/applications/com.openvireo.OpenVireo.desktop"
    mkdir -p "$(dirname "$desktop_target")"

    # Substitute Exec and TryExec in memory
    desktop_content=$(sed "s|Exec=openvireo %f|Exec=$APPIMAGE %f|;s|TryExec=openvireo|TryExec=$APPIMAGE|" "$desktop_source")

    # Copy desktop file if different or missing
    if [ ! -f "$desktop_target" ] || ! cmp -s - "$desktop_target" <<<"$desktop_content"; then
        echo "Installing OpenVireo desktop file to ${desktop_target}"
        printf '%s\n' "$desktop_content" >"$desktop_target"

        if command -v update-desktop-database &>/dev/null; then
            echo "Updating desktop database"
            update-desktop-database "${XDG_DATA_HOME:-$HOME/.local/share}/applications"
        fi
    fi

    EXEC="exec"
    if command -v systemd-run &>/dev/null; then
        EXEC="exec systemd-run --user --scope --slice=app.slice --unit=app-com.openvireo.OpenVireo-$(cat /proc/sys/kernel/random/uuid | tr -d -).scope"
    fi

    $EXEC openvireo "$@"
fi
