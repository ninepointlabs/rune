# Packaging

`cmake --install` puts the `rune` binary, `rune.desktop`, the icon and the
license under the install prefix. Packages are built from that.

## Arch

`packaging/arch/PKGBUILD` builds the tagged release from GitHub:

    cd packaging/arch && makepkg -s

## Debian / Ubuntu and Fedora (CPack)

Build inside the target distro (a container is fine) so the package links
against that distro's Qt:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    cd build && cpack -G DEB    # needs dpkg-dev for dependency detection
    cd build && cpack -G RPM    # needs rpm-build

`build-release.sh` does this in Docker for every target and collects the
results in `dist/`.
