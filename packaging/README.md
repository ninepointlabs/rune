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

## AppImage (every other distro)

`build-appimage.sh` builds `dist/rune-<version>-x86_64.AppImage` (with its
`.zsync` and `.sha256`) in an Ubuntu 22.04 container, so it runs on glibc
2.34 or newer:

    packaging/build-appimage.sh --test   # --test also runs appimage/smoke.sh

`appimage/build.sh` does the work: Qt 6.12 from aqtinstall, QuaZip built
against it, Rune, then linuxdeploy and its Qt plugin to copy in the libraries,
Qt plugins and QML modules, and appimagetool. Every download is pinned. The
hicolor PNGs in `icons/` are rendered from `rune.svg` by `icons/render.sh`.

CI (`.github/workflows/appimage.yml`) builds and checks it on pull requests
that touch packaging, and attaches it to each published release next to the
other packages, adding its line to `SHA256SUMS`. To attach one to an existing
release, run the workflow by hand with the tag.

When bumping the version, add a `<release>` line to `rune.appdata.xml` too;
the AppImage build stops if it is missing.
