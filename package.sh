#!/bin/bash
# Cross-build rkwifibt-dev-tools (BT attach tools + AIC8800 wifi/bt runtime
# payload) into an arm64 .deb using the component's own debian/ (meson).
#
# Bookworm native cross (see os-next/docs/userspace-deb-packaging.md §3). Only
# the userspace layer is here; kernel .ko + firmware ship via the BSP.
#
# Intended to run inside `container: debian:bookworm`. Produces, at repo root:
#   rkwifibt-dev-tools_<ver>_arm64.deb (+ .sha256)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_ROOT"

if [ "${PACKAGE_INSTALL_DEPS:-1}" = "1" ]; then
  export DEBIAN_FRONTEND=noninteractive
  dpkg --add-architecture arm64
  apt-get update -qq
  apt-get install -y --no-install-recommends \
    ca-certificates dpkg-dev build-essential debhelper meson ninja-build pkg-config fakeroot \
    dpkg-cross crossbuild-essential-arm64 \
    libc6:arm64 libstdc++6:arm64
fi

export DEB_BUILD_OPTIONS="noautodbgsym"
dpkg-buildpackage -a arm64 -b -uc -us

mv ../*.deb "$REPO_ROOT"/ 2>/dev/null || true
for f in "$REPO_ROOT"/*.deb; do
  ( cd "$REPO_ROOT" && sha256sum "$(basename "$f")" > "$(basename "$f").sha256" )
done

echo "== produced =="
ls -1 "$REPO_ROOT"/*.deb "$REPO_ROOT"/*.sha256
