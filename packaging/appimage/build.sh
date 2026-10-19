#!/usr/bin/env bash
# Build Rune's AppImage into dist/: Rune with its own Qt, QuaZip, OpenSSL and
# libsecret, so it runs on any x86_64 Linux with glibc 2.34 or newer (Ubuntu
# 22.04, Debian 12, Fedora 36+, ...).
#
#   packaging/appimage/build.sh [--out <dir>]
#
# Runs on Ubuntu 22.04: CI's runner, or packaging/build-appimage.sh in Docker.
# Needs what packaging/appimage/apt-packages.txt lists.
#
# Qt 6.12 comes from aqtinstall (Qt's own binaries), QuaZip is built against
# it, then linuxdeploy and its Qt plugin copy in every library and Qt plugin
# Rune uses, and appimagetool packs the result. Every download is pinned by
# version, and checked against its sha256 where it is a single file.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$here/dist"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) out="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

for tool in curl ninja patchelf zsyncmake sha256sum python3; do
  command -v "$tool" >/dev/null || { echo "build.sh needs $tool" >&2; exit 1; }
done

version="$(sed -n 's/^project(rune VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "No version in CMakeLists.txt" >&2; exit 1; }
grep -q "<release version=\"$version\"" "$here/packaging/rune.appdata.xml" \
  || { echo "packaging/rune.appdata.xml has no <release> for $version; add one." >&2; exit 1; }
name="rune-$version-x86_64.AppImage"
work="$here/build/appimage"
cache="$work/cache"
appdir="$work/Rune.AppDir"
mkdir -p "$cache" "$out"

# -- pinned tools -------------------------------------------------------------

qt_version=6.12.0  # the Qt Omarchy ships
aqtinstall_version=3.3.0
cmake_version=3.31.6  # Qt 6.12 needs 3.25+; Ubuntu 22.04 has 3.22
quazip_url=https://github.com/stachenov/quazip/archive/refs/tags/v1.5.tar.gz
quazip_sha=405b72b6e76c8987ff41a762523b8f64876ba406d8a831d268ee0b63f1369582
linuxdeploy_url=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
linuxdeploy_sha=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
plugin_qt_url=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage
plugin_qt_sha=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
appimagetool_url=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
appimagetool_sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
runtime_url=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
runtime_sha=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

fetch() {  # fetch <url> <sha256> <file>
  local file="$cache/$3"
  if [[ ! -f "$file" ]] || ! echo "$2  $file" | sha256sum -c --quiet - 2>/dev/null; then
    curl -fsSL --retry 3 -o "$file.part" "$1"
    echo "$2  $file.part" | sha256sum -c --quiet -
    mv "$file.part" "$file"
  fi
  chmod +x "$file"
}
fetch "$quazip_url" "$quazip_sha" quazip.tar.gz
fetch "$linuxdeploy_url" "$linuxdeploy_sha" linuxdeploy-x86_64.AppImage
fetch "$plugin_qt_url" "$plugin_qt_sha" linuxdeploy-plugin-qt-x86_64.AppImage
fetch "$appimagetool_url" "$appimagetool_sha" appimagetool-x86_64.AppImage
fetch "$runtime_url" "$runtime_sha" runtime-x86_64
# The AppImage tools run without FUSE (containers, CI).
export APPIMAGE_EXTRACT_AND_RUN=1

echo "== Qt $qt_version, CMake $cmake_version"
tools="$cache/tools"
if [[ ! -x "$tools/bin/aqt" || ! -x "$tools/bin/cmake" ]]; then
  python3 -m venv "$tools"
  "$tools/bin/pip" install -q "aqtinstall==$aqtinstall_version" "cmake==$cmake_version"
fi
export PATH="$tools/bin:$PATH"
# Only what Rune builds against; qtsvg for the Wayland title bars' icons,
# qt5compat because QuaZip's Qt 6 build uses it.
qt_parts="--archives icu qtbase qtdeclarative qtwayland qtsvg -m qt5compat"
qt="$cache/qt/$qt_version/gcc_64"
if [[ "$(cat "$cache/qt/parts" 2>/dev/null)" != "$qt_parts" ]]; then
  rm -rf "$cache/qt" "$cache/quazip"
  # shellcheck disable=SC2086  # $qt_parts is a list of words
  (cd "$work" && aqt install-qt linux desktop "$qt_version" linux_gcc_64 $qt_parts -O "$cache/qt") >"$work/aqt.log" 2>&1 \
    || { cat "$work/aqt.log" >&2; exit 1; }
  echo "$qt_parts" >"$cache/qt/parts"
fi

echo "== QuaZip"
quazip="$cache/quazip"
if [[ ! -f "$quazip/lib/cmake/QuaZip-Qt6-1.5/QuaZip-Qt6Config.cmake" ]]; then
  rm -rf "$work/quazip-src" "$work/quazip-build"
  mkdir -p "$work/quazip-src"
  tar -xzf "$cache/quazip.tar.gz" -C "$work/quazip-src" --strip-components=1
  cmake -S "$work/quazip-src" -B "$work/quazip-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$qt" -DCMAKE_INSTALL_PREFIX="$quazip" -DQUAZIP_QT_MAJOR_VERSION=6 \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DQUAZIP_ENABLE_TESTS=OFF \
    -DQUAZIP_FETCH_LIBS=OFF -DQUAZIP_BZIP2=OFF >/dev/null
  cmake --build "$work/quazip-build" >/dev/null
  cmake --install "$work/quazip-build" >/dev/null
fi

echo "== Rune $version"
rm -rf "$appdir"
cmake -S "$here" -B "$work/rune-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$qt;$quazip" -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$work/rune-build"
DESTDIR="$appdir" cmake --install "$work/rune-build" >/dev/null
# smoke.sh's TLS check; not part of the AppImage.
cmake -S "$here/packaging/appimage/tls-probe" -B "$work/tls-probe" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$qt" >/dev/null
cmake --build "$work/tls-probe" >/dev/null

echo "== Desktop entry, icons, metainfo"
share="$appdir/usr/share"
echo "X-AppImage-Version=$version" >>"$share/applications/rune.desktop"
install -Dm644 "$here/packaging/rune.appdata.xml" "$share/metainfo/rune.appdata.xml"
cp -r "$here/packaging/icons/hicolor" "$share/icons/"

echo "== Libraries and Qt plugins"
# linuxdeploy copies every library Rune links to (except what its excludelist
# says must come from the host: libc, GL drivers, fontconfig...) and points
# RUNPATHs at them; its Qt plugin adds the QML modules Rune's QML imports, the
# Wayland and X11 platform plugins and the rest, and writes a qt.conf, so no
# environment variable is needed to find any of it.
# libssl is loaded at run time by Qt's TLS plugin, so it is named here.
# No platform themes (the GTK 3 one would drag GTK in): file dialogs are Qt
# Quick's own, light/dark comes from the portal (main.cpp).
# The plugin knows the Wayland platform plugin but not, for Qt 6.12, the shell,
# decoration and graphics plugins it loads (xdg-shell, client-side title bars
# on GNOME, EGL), so they are put in place first and only their libraries deployed.
deps_only=()
for dir in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
  mkdir -p "$appdir/usr/plugins/$dir"
  cp "$qt/plugins/$dir"/*.so "$appdir/usr/plugins/$dir/"
  deps_only+=(--deploy-deps-only "$appdir/usr/plugins/$dir")
done
export QMAKE="$qt/bin/qmake" LD_LIBRARY_PATH="$qt/lib:$quazip/lib" \
  QML_SOURCES_PATHS="$here/native" EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so" EXTRA_QT_MODULES="waylandclient"
(cd "$work" && "$cache/linuxdeploy-x86_64.AppImage" --appdir "$appdir" \
  --desktop-file "$share/applications/rune.desktop" \
  --icon-file "$share/icons/hicolor/256x256/apps/rune.png" --icon-file "$here/packaging/rune.svg" \
  --library "$(ldconfig -p | awk '$1 == "libssl.so.3" && /x86-64/ && !found { print $NF; found = 1 }')" \
  "${deps_only[@]}" --plugin qt) >"$work/linuxdeploy.log" 2>&1 \
  || { cat "$work/linuxdeploy.log" >&2; exit 1; }
unset LD_LIBRARY_PATH
grep -E '^(\[qt/stdout\] )?(-- |ERROR)' "$work/linuxdeploy.log" || true
# linuxdeploy carries on past a library it cannot find; a bundle missing one is broken.
if grep -E 'ERROR: Could not find dependency' "$work/linuxdeploy.log"; then
  echo "Install the library on the build machine (packaging/appimage/apt-packages.txt) and build again." >&2
  exit 1
fi
# Rune's own launcher, in place of linuxdeploy's: that one runs the Qt plugin's
# hook, which sets QT_QPA_PLATFORMTHEME=gtk3 on GNOME (a theme not bundled
# here) for Rune and for everything Rune starts.
rm -rf "$appdir/apprun-hooks" "$appdir/AppRun" "$appdir/AppRun.wrapped"
install -m755 "$here/packaging/appimage/AppRun" "$appdir/AppRun"

# QML modules and Controls styles the plugin copies whole but Rune never loads.
# Rune's controls are Basic; Fusion stays for Qt Quick's own file dialog.
rm -rf "$appdir"/usr/qml/QtQuick/{Particles,LocalStorage,tooling} "$appdir"/usr/qml/QtQml/{XmlListModel,DesignSupport} \
  "$appdir"/usr/qml/QtQuick/Controls/{Material,Universal,Imagine,FluentWinUI3}
rm -f "$appdir"/usr/lib/libQt6{QuickParticles,QmlLocalStorage,Sql,QmlXmlListModel,QmlDesignSupport}.so* \
  "$appdir"/usr/lib/libQt6QuickControls2{Material,Universal,Imagine,FluentWinUI3}{,StyleImpl}.so*
ln -sf usr/share/icons/hicolor/256x256/apps/rune.png "$appdir/.DirIcon"

echo "== AppImage"
rm -f "$out/$name" "$out/$name.zsync" "$out/$name.sha256"
(cd "$out" && ARCH=x86_64 "$cache/appimagetool-x86_64.AppImage" --no-appstream \
  --runtime-file "$cache/runtime-x86_64" \
  -u "gh-releases-zsync|ninepointlabs|rune|latest|rune-*x86_64.AppImage.zsync" \
  "$appdir" "$out/$name")
(cd "$out" && sha256sum "$name" >"$name.sha256")
ls -l "$out/$name" "$out/$name.zsync" "$out/$name.sha256"
du -sh "$appdir" | sed 's/^/   unpacked: /'
