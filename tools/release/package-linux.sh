#!/usr/bin/env bash
# Packages a Linux build for a release: an AppImage and a portable tar.gz.
#   tools/release/package-linux.sh <version> <build dir> <output dir>
# Needs linuxdeploy on PATH (or LINUXDEPLOY=<path>). Run from the repository root.
set -euo pipefail
version=$1 build=$2 out=$3
name="Moonsprout-${version}-linux-x86_64"
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

notices() { # $1: destination folder
    cp LICENSE.MD "$1/LICENSE.txt"
    tools/release/collect-licenses.sh "$build" "$1/licenses" >/dev/null
    cp tools/release/README-release.txt "$1/README.txt"
}

# ---- Portable tar.gz: moonsprout, res/ and the notices in one folder.
dir="$work/Moonsprout"
mkdir -p "$dir"
cp "$build/mppm" "$dir/moonsprout"
strip --strip-debug "$dir/moonsprout"
cp -r "$build/res" "$dir/res"
notices "$dir"
tar -C "$work" -czf "$out/$name.tar.gz" Moonsprout

# ---- AppImage. The game finds res/ next to its executable (SDL_GetBasePath).
app="$work/AppDir"
mkdir -p "$app/usr/bin" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/256x256/apps"
cp "$dir/moonsprout" "$app/usr/bin/moonsprout"
cp -r "$build/res" "$app/usr/bin/res"
mkdir -p "$app/usr/share/doc/moonsprout"
notices "$app/usr/share/doc/moonsprout"
cp packaging/linux/moonsprout.desktop "$app/usr/share/applications/"
cp port/launcher/res/img/icon-256.png "$app/usr/share/icons/hicolor/256x256/apps/moonsprout.png"
export NO_STRIP=1 APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 LINUXDEPLOY_OUTPUT_VERSION="$version" LDAI_OUTPUT="$out/$name.AppImage"
"${LINUXDEPLOY:-linuxdeploy}" --appdir "$app" --output appimage \
    --desktop-file "$app/usr/share/applications/moonsprout.desktop" \
    --icon-file "$app/usr/share/icons/hicolor/256x256/apps/moonsprout.png"
ls -l "$out"
