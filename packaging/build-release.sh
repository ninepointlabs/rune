#!/bin/bash
# Build the .deb and .rpm release packages in Docker, one container per
# target distro, and collect them in dist/. The Arch package is built with
# makepkg from packaging/arch/ (see README.md).
#
#   packaging/build-release.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
mkdir -p dist

build() { # image, install command, cpack generator
    docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/dist" "$1" bash -euc "
        $2
        cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build /tmp/build
        cd /tmp/build && cpack -G $3
        cp /tmp/build/*.${3,,} /dist/
        chown $(id -u):$(id -g) /dist/*.${3,,}"
}

build debian:trixie "
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        cmake ninja-build g++ pkg-config dpkg-dev file \
        qt6-base-dev qt6-declarative-dev libquazip1-qt6-dev libssl-dev libsecret-1-dev" DEB

build fedora:42 "
    dnf install -y -q cmake ninja-build gcc-c++ pkgconf-pkg-config rpm-build \
        qt6-qtbase-devel qt6-qtdeclarative-devel quazip-qt6-devel openssl-devel libsecret-devel" RPM

ls -l dist
