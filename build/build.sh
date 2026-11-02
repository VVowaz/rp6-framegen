#!/bin/sh
# Builds the Vulkan layer and the offline bench in a podman container (Fedora toolchain).
# usage: build.sh [OUTDIR]      (as a normal user; needs podman; default OUTDIR is <repo>/out)
set -e
REPO=$(dirname "$(dirname "$(readlink -f "$0")")")
OUT=${1:-$REPO/out}
mkdir -p "$OUT"
podman build -q -t localhost/rp6fg-build -f "$REPO/build/Containerfile" "$REPO/build" >/dev/null
podman run --rm -v "$REPO/layer":/src:ro,z -v "$OUT":/out:z localhost/rp6fg-build \
	sh -c 'cmake -S /src -B /out/build -G Ninja >/dev/null && cmake --build /out/build'
echo "built: $OUT/build/librp6fg.so, $OUT/build/rp6fg-bench"
