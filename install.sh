#!/bin/sh
# RP6 FrameGen installer for the Retroid Pocket 6 running the Armada Linux image.
#
#   sudo ./install.sh [--user NAME] [--skip-build]
#
# --user        the desktop user (default: the user that ran sudo)
# --skip-build  reuse out/build/librp6fg.so from a previous build
# Builds the Vulkan layer in podman and installs the Decky plugin; the plugin installs the layer into the
# user's ~/.local on load. Idempotent: running it again updates everything and keeps your profiles.
set -e
SKIP_BUILD=0; USER_NAME=${SUDO_USER:-}
while [ $# -gt 0 ]; do
	case "$1" in
	--user) USER_NAME=$2; shift ;;
	--skip-build) SKIP_BUILD=1 ;;
	-h|--help) sed -n '2,10p' "$0"; exit 0 ;;
	*) echo "unknown option $1" >&2; exit 1 ;;
	esac
	shift
done
HERE=$(dirname "$(readlink -f "$0")")
die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || die "cannot tell the desktop user; pass --user NAME"
HOME_D=$(getent passwd "$USER_NAME" | cut -d: -f6)
[ "$(uname -m)" = aarch64 ] || die "aarch64 only"
command -v podman >/dev/null || die "podman is required"
[ -d "$HOME_D/homebrew/plugins" ] || die "Decky Loader not found ($HOME_D/homebrew/plugins)"

if [ "$SKIP_BUILD" = 0 ]; then
	echo "== building the layer (podman)"
	chown -R "$USER_NAME": "$HERE"
	# podman needs the user's own runtime dir and home, not root's
	runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="/run/user/$(id -u "$USER_NAME")" HOME="$HOME_D" \
		sh "$HERE/build/build.sh" "$HERE/out"
fi
LIB=$HERE/out/build/librp6fg.so
[ -f "$LIB" ] || die "$LIB missing; run without --skip-build"

echo "== Decky plugin"
PL=$HOME_D/homebrew/plugins/rp6-framegen
rm -rf "$PL"; mkdir -p "$PL/bin"
cp -r "$HERE/decky/rp6-framegen/." "$PL/"
install -m 755 "$LIB" "$PL/bin/librp6fg.so"
install -m 644 "$HERE/layer/rp6fg_layer.json.in" "$PL/bin/rp6fg_layer.json.in"
systemctl restart plugin_loader 2>/dev/null || echo "restart Decky (plugin_loader) to load the plugin"
echo "done: open the Decky menu -> RP6 FrameGen, pick a game, turn on Frame generation, start the game"
