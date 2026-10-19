#!/bin/bash
# Build the AppImage into dist/ in an Ubuntu 22.04 container, the way CI does
# (.github/workflows/appimage.yml). The .deb, .rpm and Arch packages are
# build-release.sh's and makepkg's, unchanged.
#
#   packaging/build-appimage.sh [--test]
#
# --test   then run packaging/appimage/smoke.sh on it, in the same container.
#
# Needs Docker. Builds the working tree; Qt and the tools are cached in
# build/appimage/cache.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
test=0
case "${1:-}" in
    --test) test=1 ;;
    "") ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
esac

docker run --rm -v "$PWD:/src" -w /src -e OWNER="$(id -u):$(id -g)" -e TEST="$test" ubuntu:22.04 bash -euc '
    trap "chown -R \"\$OWNER\" dist build 2>/dev/null || true" EXIT
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    sed "s/#.*//" packaging/appimage/apt-packages.txt | xargs apt-get install -y -qq --no-install-recommends >/dev/null
    packaging/appimage/build.sh
    if [[ "$TEST" == 1 ]]; then
        packaging/appimage/smoke.sh dist/rune-*-x86_64.AppImage
    fi'
