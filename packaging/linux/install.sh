#!/bin/sh
# Per-user install of the mic thingy launcher (application menu / Super-key search)
# and, optionally, start at login. No root needed; nothing is written outside $HOME.
#
#   packaging/linux/install.sh [--exe PATH] [--autostart] [--uninstall]
#
# --exe PATH    the mic-thingy binary (default: build/mic-thingy next to the repo root)
# --autostart   also start at login, hidden (mic-thingy --autostart --minimized)
# --uninstall   remove everything this script installed
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
exe="$root/build/mic-thingy"
autostart=0
uninstall=0

while [ $# -gt 0 ]; do
    case "$1" in
        --exe) [ $# -ge 2 ] || { echo "--exe needs a path" >&2; exit 2; }
               exe=$(realpath "$2"); shift ;;
        --autostart) autostart=1 ;;
        --uninstall) uninstall=1 ;;
        -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

data=${XDG_DATA_HOME:-$HOME/.local/share}
conf=${XDG_CONFIG_HOME:-$HOME/.config}
app_file="$data/applications/mic-thingy.desktop"
icon_file="$data/icons/hicolor/256x256/apps/mic-thingy.png"
auto_file="$conf/autostart/mic-thingy.desktop"

refresh() {
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$data/applications" 2>/dev/null || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$data/icons/hicolor" >/dev/null 2>&1 || true
    if command -v kbuildsycoca6 >/dev/null 2>&1; then kbuildsycoca6 >/dev/null 2>&1 || true
    elif command -v kbuildsycoca5 >/dev/null 2>&1; then kbuildsycoca5 >/dev/null 2>&1 || true; fi
}

if [ "$uninstall" = 1 ]; then
    rm -f "$app_file" "$icon_file" "$auto_file"
    refresh
    echo "removed launcher, icon and autostart entry"
    exit 0
fi

[ -x "$exe" ] || { echo "binary not found: $exe (build first, or pass --exe PATH)" >&2; exit 1; }
case "$exe" in *[\"\\\`\$]*) echo "path contains characters not allowed in a .desktop Exec line" >&2; exit 1 ;; esac

install_desktop() {  # $1 = destination, $2 = args, $3 = extra lines
    mkdir -p "$(dirname "$1")"
    awk -v exe="$exe" -v args="$2" -v extra="$3" '
        { gsub(/@EXE@/, exe); gsub(/@ARGS@/, args); gsub(/@EXTRA@/, extra); print }
    ' "$here/mic-thingy.desktop.in" > "$1"
}

mkdir -p "$(dirname "$icon_file")"
cp "$root/assets/logo-256.png" "$icon_file"
install_desktop "$app_file" "" ""
echo "installed: $app_file"
if [ "$autostart" = 1 ]; then
    install_desktop "$auto_file" " --autostart --minimized" "X-GNOME-Autostart-enabled=true"
    echo "installed: $auto_file (starts hidden at login)"
fi
refresh
echo "done - search for \"mic thingy\" in the application menu"
