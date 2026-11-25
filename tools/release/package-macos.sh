#!/usr/bin/env bash
# Packages a macOS build for a release as a drag-to-Applications disk image.
#   tools/release/package-macos.sh <version> <build dir> <output dir> <arch: arm64|x86_64>
# Run from the repository root on macOS. The app is ad-hoc signed, not notarized.
set -euo pipefail
version=$1 build=$2 out=$3 arch=$4
name="Moonsprout-${version}-macos-${arch}"
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

app="$work/dmg/Moonsprout.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$build/mppm" "$app/Contents/MacOS/Moonsprout"
strip -S "$app/Contents/MacOS/Moonsprout"
# Aurora's resource folder in a bundle is Contents/Resources (SDL_GetBasePath).
cp -r "$build/res" "$app/Contents/Resources/res"
short=${version%%-*}
sed -e "s/@SHORT_VERSION@/$short/" -e "s/@VERSION@/$version/" packaging/macos/Info.plist.in > "$app/Contents/Info.plist"

iconset="$work/Moonsprout.iconset"
mkdir -p "$iconset"
src=port/launcher/res/img/icon-1024.png
for s in 16 32 128 256 512; do
    sips -z $s $s "$src" --out "$iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$src" --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/Moonsprout.icns"

cp LICENSE.MD "$app/Contents/Resources/LICENSE.txt"
tools/release/collect-licenses.sh "$build" "$app/Contents/Resources/licenses"

# Any dylibs the build left next to the binary (none expected; Aurora links statically).
for lib in "$build"/*.dylib; do
    [ -e "$lib" ] && cp "$lib" "$app/Contents/MacOS/"
done
# Only system libraries may be linked: a Homebrew dylib would be missing on players' Macs.
if otool -L "$app/Contents/MacOS/Moonsprout" | tail -n +2 | grep -vE '^\s+(/usr/lib/|/System/Library/|@rpath/|@executable_path/)'; then
    echo "error: the binary links libraries outside the system (above)" >&2
    exit 1
fi
codesign --force --deep --sign - "$app"

cp tools/release/README-release.txt "$work/dmg/README.txt"
cp "$app/Contents/Resources/licenses/THIRD-PARTY-NOTICES.txt" "$work/dmg/"
ln -s /Applications "$work/dmg/Applications"
hdiutil create -volname "Moonsprout" -srcfolder "$work/dmg" -ov -format UDZO "$out/$name.dmg"
ls -l "$out"
