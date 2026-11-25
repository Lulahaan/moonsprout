#!/usr/bin/env bash
# Writes the release notes (Markdown) to stdout: download links grouped by platform first, then the
# changes since the previous release (GitHub's generated notes), then the SHA-256 checksums.
#   tools/release/notes.sh <tag> <dist dir> <owner/repo> [extra notes file]
# <dist dir> holds the release files and SHA256SUMS.txt. Needs gh (GH_TOKEN) for the changes list.
set -euo pipefail
tag=$1 dist=$2 repo=$3 extra=${4:-}
version=${tag#v}
base="https://github.com/$repo/releases/download/$tag"

link() { # $1 label, $2 file name
    if [ -f "$dist/$2" ]; then
        local size
        size=$(du -m "$dist/$2" | cut -f1)
        echo "- **[$1]($base/$2)** · \`$2\` · ${size} MB"
    fi
}

echo "## Downloads"
echo
echo "### Windows (x64, Windows 10/11)"
link "Installer" "Moonsprout-$version-windows-x64-setup.exe"
link "Portable (zip)" "Moonsprout-$version-windows-x64-portable.zip"
echo
echo "### Linux (x86_64)"
link "AppImage" "Moonsprout-$version-linux-x86_64.AppImage"
link "Portable (tar.gz)" "Moonsprout-$version-linux-x86_64.tar.gz"
echo
echo "### macOS (13 or newer)"
link "Apple silicon (dmg)" "Moonsprout-$version-macos-arm64.dmg"
link "Intel (dmg)" "Moonsprout-$version-macos-x86_64.dmg"
echo
if [[ "$tag" == *-* ]]; then
    echo "> [!WARNING]"
    echo "> This is a **prerelease** for testing. Expect bugs, and back up your saves (\`Settings → Prelaunch → Save Manager\`) before you play."
    echo
fi
echo "> [!IMPORTANT]"
echo "> Moonsprout contains no game data. You need a disc image of **Pikmin 2 (USA, \`GPVE01\`)** dumped from your own disc."
echo "> The builds are not code-signed yet: on Windows choose **More info → Run anyway**; on macOS right-click the app and choose **Open**"
echo "> (or run \`xattr -dr com.apple.quarantine /Applications/Moonsprout.app\`). The AppImage needs \`chmod +x\` and glibc 2.39 or newer."
echo

if [ -n "$extra" ] && [ -s "$extra" ]; then
    cat "$extra"
    echo
fi

# Changes since the previous tag of the same kind (any tag for a prerelease, a full release for a release).
prev=""
if [[ "$tag" == *-* ]]; then
    prev=$(git tag --list 'v*' --sort=-v:refname | grep -v -x -F "$tag" | head -1 || true)
else
    prev=$(git tag --list 'v*' --sort=-v:refname | grep -v -- '-' | grep -v -x -F "$tag" | head -1 || true)
fi
args=(-f "tag_name=$tag")
[ -n "$prev" ] && args+=(-f "previous_tag_name=$prev")
if body=$(gh api "repos/$repo/releases/generate-notes" "${args[@]}" --jq .body 2>/dev/null); then
    echo "$body"
    echo
fi

if [ -f "$dist/SHA256SUMS.txt" ]; then
    echo "<details><summary>SHA-256 checksums</summary>"
    echo
    echo '```'
    cat "$dist/SHA256SUMS.txt"
    echo '```'
    echo
    echo "</details>"
fi
