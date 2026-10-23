#!/usr/bin/env bash
# Check a built AppImage the way someone on another distro would meet it.
#
#   packaging/appimage/smoke.sh dist/rune-<version>-x86_64.AppImage
#
# Runs without FUSE (APPIMAGE_EXTRACT_AND_RUN), so it works in containers and CI.
# Needs what packaging/appimage/apt-packages.txt lists (Xvfb, Weston, xdotool...).
# Rune's own --auto-test needs LibreOffice and a checkout of the repo, so it is
# run separately (see .github/workflows/appimage.yml).

# ok() never fails, so `check && ok ... || fail ...` is if/else here.
# shellcheck disable=SC2015
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
image="$(readlink -f "${1:?usage: smoke.sh <AppImage>}")"
export APPIMAGE_EXTRACT_AND_RUN=1 LANG=C.UTF-8
work="$(mktemp -d)"
trap 'kill "${xvfb_pid:-}" "${weston_pid:-}" 2>/dev/null || true; rm -rf "$work"' EXIT
cd "$work"
failures=0
ok() { echo "ok: $*"; }
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }
# Words that mean something went wrong at start-up (missing plugin, QML module, library...).
problems='error|not installed|cannot|failed|could not|no such|undefined symbol'

"$image" --appimage-extract >/dev/null
appdir="$work/squashfs-root"

echo "== Desktop entry, metainfo and icons inside"
desktop-file-validate "$appdir/rune.desktop" && desktop-file-validate "$appdir/usr/share/applications/rune.desktop" \
  && ok "desktop files validate" || fail "desktop-file-validate"
grep -q '^X-AppImage-Version=' "$appdir/rune.desktop" && ok "X-AppImage-Version set" || fail "no X-AppImage-Version"
metainfo=("$appdir"/usr/share/metainfo/*.xml)
if [[ ${#metainfo[@]} == 1 && -f "${metainfo[0]}" ]] && appstreamcli validate --no-net "${metainfo[0]}"; then
  ok "metainfo validates: ${metainfo[0]##*/}"
else
  fail "want one valid metainfo file, have: ${metainfo[*]##*/}"
fi
for size in 16 24 32 48 64 128 256 512; do
  png="$appdir/usr/share/icons/hicolor/${size}x${size}/apps/rune.png"
  file "$png" | grep -q "PNG image data, $size x $size" || fail "icon $size: $(file -b "$png" 2>&1)"
done
[[ -f "$appdir/usr/share/icons/hicolor/scalable/apps/rune.svg" && -f "$appdir/rune.svg" ]] || fail "SVG icon missing"
file -L "$appdir/.DirIcon" | grep -q "PNG image data, 256 x 256" || fail ".DirIcon is not the 256px PNG"
ok "icons: 16-512 PNGs, scalable SVG, root icon, .DirIcon"
for dir in platforms/libqwayland.so platforms/libqxcb.so wayland-shell-integration/libxdg-shell.so \
  wayland-decoration-client/libbradient.so wayland-graphics-integration-client/libqt-plugin-wayland-egl.so \
  tls/libqopensslbackend.so; do
  [[ -f "$appdir/usr/plugins/$dir" ]] || fail "Qt plugin missing: $dir"
done
[[ -f "$appdir/usr/lib/libssl.so.3" ]] && ok "Wayland, X11 and TLS plugins and libssl bundled" || fail "libssl not bundled"
[[ -f "$appdir/usr/plugins/platformthemes/libqxdgdesktopportal.so" ]] && ok "portal platform theme bundled" \
  || fail "platformthemes/libqxdgdesktopportal.so missing"
bundled_xkb="$(find "$appdir/usr/lib" -name 'libxkbcommon*' 2>/dev/null)"
[[ -z "$bundled_xkb" ]] && ok "libxkbcommon left to the host" || fail "libxkbcommon bundled: $bundled_xkb"

echo "== Command line"
# Rune makes its QGuiApplication before reading options, so even these need a display.
version="$(QT_QPA_PLATFORM=offscreen "$image" --version)"
grep -q '^Rune [0-9]' <<<"$version" && ok "--version: $version" || fail "--version: $version"
QT_QPA_PLATFORM=offscreen "$image" --help | grep -q -- --version && ok "--help" || fail "--help"

echo "== The window, offscreen"
set +e
QT_QPA_PLATFORM=offscreen timeout 8 "$image" >offscreen.log 2>&1
rc=$?
set -e
if [[ $rc == 124 ]] && ! grep -Eiq "$problems" offscreen.log; then
  ok "window starts offscreen"
else
  fail "offscreen window (exit $rc): $(cat offscreen.log)"
fi

echo "== TLS: the bundled OpenSSL and the host's CA certificates"
# build.sh builds the probe against the same Qt; it runs from the AppDir like Rune.
probe="${TLS_PROBE:-$here/build/appimage/tls-probe/tls-probe}"
if [[ -x "$probe" ]]; then
  cp "$probe" "$appdir/usr/bin/tls-probe"
  if report="$("$appdir/usr/bin/tls-probe" 2>&1)" && grep -q "OpenSSL 3" <<<"$report"; then
    ok "TLS: $report"
  else
    fail "TLS: $report"
  fi
  rm "$appdir/usr/bin/tls-probe"
else
  echo "skip: no TLS probe at $probe (packaging/appimage/build.sh makes it)"
fi

echo "== The window on X11 (Xvfb), with no Omarchy, no Hyprland and an empty home"
Xvfb :77 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
xvfb_pid=$!
sleep 1
fresh_home="$(mktemp -d)"
env -i DISPLAY=:77 HOME="$fresh_home" PATH=/usr/bin:/bin APPIMAGE_EXTRACT_AND_RUN=1 LANG=C.UTF-8 \
  "$image" >x11.log 2>&1 &
app_pid=$!
wid="$(DISPLAY=:77 timeout 30 xdotool search --sync --onlyvisible --classname '^rune$' 2>/dev/null | head -1 || true)"
if [[ -n "$wid" ]]; then
  wm_class="$(DISPLAY=:77 xprop -id "$wid" WM_CLASS)"
  wanted="$(sed -n 's/^StartupWMClass=//p' "$appdir/rune.desktop")"
  if [[ -n "$wanted" ]] && grep -q "\"$wanted\"" <<<"$wm_class"; then
    ok "WM_CLASS ($wm_class) has StartupWMClass=$wanted"
  else
    fail "WM_CLASS $wm_class does not match StartupWMClass=$wanted"
  fi
  icon=""
  for w in $(DISPLAY=:77 xdotool search --classname '^rune$'); do
    icon+="$(DISPLAY=:77 xprop -id "$w" -len 12 _NET_WM_ICON | grep -o 'Icon ([0-9]* x [0-9]*)' || true)"
  done
  if [[ -n "$icon" ]]; then
    ok "the window carries its own icon: $icon"
  elif [[ "${SMOKE_LEGACY:-0}" == 1 ]]; then
    echo "warning: no _NET_WM_ICON on the window (this version predates it)"
  else
    fail "no _NET_WM_ICON on the window"
  fi
  win_pid="$(DISPLAY=:77 xdotool getwindowpid "$wid")"
  leaked="$(tr '\0' '\n' <"/proc/$win_pid/environ" | grep -E '^(LD_LIBRARY_PATH|LD_PRELOAD|QT_PLUGIN_PATH|QML2_IMPORT_PATH|QML_IMPORT_PATH|QT_QPA_PLATFORMTHEME|XDG_DATA_DIRS|SSL_CERT_FILE)=' || true)"
  [[ -z "$leaked" ]] && ok "the app's environment (inherited by soffice, the browser...) has nothing from the bundle" \
    || fail "bundle variables leak to child processes: $leaked"
  sleep 2
else
  fail "no window appeared on Xvfb: $(cat x11.log)"
fi
kill "$app_pid" 2>/dev/null || true
wait "$app_pid" 2>/dev/null || true
if grep -Eiq "$problems" x11.log; then fail "X11 run printed: $(cat x11.log)"; else ok "X11 run is quiet (theme fell back, nothing missing)"; fi
[[ ! -e "$fresh_home/.local/state/omarchy" && ! -e "$fresh_home/.config/omarchy" && ! -e "$fresh_home/.config/hypr" ]] \
  && ok "wrote no Omarchy or Hyprland config" || fail "touched Omarchy/Hyprland config in a fresh home"

echo "== The window on Wayland (headless Weston)"
if command -v weston >/dev/null; then
  export XDG_RUNTIME_DIR="$work/runtime"
  mkdir -m 700 "$XDG_RUNTIME_DIR"
  weston --backend=headless-backend.so --socket=rune-smoke --idle-time=0 >weston.log 2>&1 &
  weston_pid=$!
  for _ in $(seq 50); do [[ -S "$XDG_RUNTIME_DIR/rune-smoke" ]] && break; sleep 0.1; done
  set +e
  env -i WAYLAND_DISPLAY=rune-smoke XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" HOME="$fresh_home" PATH=/usr/bin:/bin \
    APPIMAGE_EXTRACT_AND_RUN=1 LANG=C.UTF-8 QT_QPA_PLATFORM=wayland timeout 8 "$image" >wayland.log 2>&1
  rc=$?
  set -e
  if [[ $rc == 124 ]] && ! grep -Eiq "$problems" wayland.log; then
    ok "window starts on Wayland (xdg-shell), quietly"
  else
    fail "Wayland window (exit $rc): $(cat wayland.log)"
  fi
else
  echo "skip: weston not installed"
fi

echo "== --install-desktop-entry / --remove-desktop-entry"
data="$work/xdg-data"
XDG_DATA_HOME="$data" "$image" --install-desktop-entry
entry="$data/applications/rune.desktop"
desktop-file-validate "$entry" && ok "installed entry validates" || fail "installed entry does not validate"
grep -qx "Exec=\"$image\" %f" "$entry" && grep -qx "TryExec=$image" "$entry" \
  && ok "Exec and TryExec point at the AppImage" || fail "Exec/TryExec: $(grep -E '^(Try)?Exec=' "$entry")"
[[ -f "$data/icons/hicolor/256x256/apps/rune.png" && -f "$data/icons/hicolor/scalable/apps/rune.svg" ]] \
  && ok "icons installed" || fail "icons missing"
XDG_DATA_HOME="$data" "$image" --install-desktop-entry >/dev/null && ok "install is idempotent" || fail "second install"
XDG_DATA_HOME="$data" "$image" --remove-desktop-entry
left="$(find "$data" -type f -name 'rune*' 2>/dev/null)"
[[ -z "$left" ]] && ok "remove takes every file away" || fail "left behind: $left"
"$appdir/AppRun" --install-desktop-entry >/dev/null 2>&1 && fail "install from an extracted AppDir should refuse" \
  || ok "install from an extracted AppDir refuses kindly"

echo
if [[ $failures -gt 0 ]]; then
  echo "$failures check(s) failed." >&2
  exit 1
fi
echo "All AppImage checks passed. Size: $(du -h "$image" | cut -f1)"
