#!/bin/sh
# Quick health check of an RP6 FrameGen install (run as the desktop user).
ok() { echo "ok    $*"; }
bad() { echo "FAIL  $*"; }
[ "$(uname -m)" = aarch64 ] && ok "aarch64" || bad "not aarch64"
if vulkaninfo 2>/dev/null | grep -q VK_QCOM_image_processing; then ok "GPU has VK_QCOM_image_processing (hardware block matching)"
else bad "VK_QCOM_image_processing missing (needs Adreno + Mesa Turnip)"; fi
M=$HOME/.local/share/vulkan/implicit_layer.d/rp6fg.json
[ -f "$M" ] && ok "layer manifest $M" || bad "layer manifest missing (is the Decky plugin loaded?)"
L=$(sed -n 's/.*"library_path": "\(.*\)".*/\1/p' "$M" 2>/dev/null)
[ -n "$L" ] && [ -f "$L" ] && ok "layer library $L" || bad "layer library missing"
[ -d "$HOME/homebrew/plugins/rp6-framegen" ] && ok "Decky plugin installed" || bad "Decky plugin not installed"
systemctl is-active -q plugin_loader && ok "Decky Loader running" || bad "Decky Loader not running"
if [ -f "/dev/shm/rp6fg-live-$(id -u)" ]; then ok "live block present"; python3 -I "$(dirname "$0")/live.py" 2>/dev/null; else echo "info  no live block yet (created when a game with RP6FG=1 starts)"; fi
