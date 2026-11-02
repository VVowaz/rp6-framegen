#!/bin/sh
# Removes the Decky plugin and the installed layer. Keeps ~/.config/rp6fg (profiles); delete it by hand if wanted.
# Games keep "RP6FG=1" in their launch options; without the layer it does nothing (turn it off in the plugin first
# if you want clean launch options).
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
USER_NAME=${1:-${SUDO_USER:-}}
[ -n "$USER_NAME" ] || { echo "usage: sudo ./uninstall.sh [user]" >&2; exit 1; }
H=$(getent passwd "$USER_NAME" | cut -d: -f6)
rm -rf "$H/homebrew/plugins/rp6-framegen"
rm -f "$H/.local/share/vulkan/implicit_layer.d/rp6fg.json" "$H/.local/lib/rp6fg/librp6fg.so"
rmdir "$H/.local/lib/rp6fg" 2>/dev/null || true
rm -f "/dev/shm/rp6fg-live-$(id -u "$USER_NAME")"
systemctl restart plugin_loader 2>/dev/null || true
echo "removed"
