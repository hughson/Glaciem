#!/bin/bash
# Build a Glaciem-Miner-Linux.flatpak bundle from the current AppImage's binary tree.
#
# Run on glaciem-node (or any Ubuntu/Debian with flatpak + flatpak-builder).
# Assumes the AppImage has already been built and is extracted at /tmp/squashfs-root/.
# That happens automatically when ./rebuild-appimage.sh ran (squashfs-root is the
# extracted view of the AppImage filesystem) — if not, run:
#   /tmp/glaciem-appdir/Glaciem-Miner-Linux.AppImage --appimage-extract
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# /tmp on glaciem-node is tmpfs (1.9G) -- not enough room for a flatpak-builder
# working dir plus the ostree export. Put the build on / instead.
WORK=/root/glaciem-flatpak-build
SQUASH=/tmp/squashfs-root

# Always re-extract from the latest AppImage so we don't accidentally bundle
# a stale binary tree. The AppImage at /tmp/glaciem-appdir/ is the freshest
# one (rebuild-appimage.sh writes it there); squashfs-root may be a leftover
# extract from a previous build cycle.
APPIMAGE=/tmp/glaciem-appdir/Glaciem-Miner-Linux.AppImage
if [ ! -x "$APPIMAGE" ]; then
    echo "ERROR: $APPIMAGE not found — run rebuild-appimage.sh first." >&2
    exit 1
fi
echo "=== extract fresh AppImage to $SQUASH ==="
rm -rf "$SQUASH"
( cd /tmp && "$APPIMAGE" --appimage-extract >/dev/null )

echo "=== prepare sources ==="
rm -rf "$WORK"
mkdir -p "$WORK"
cp "$SQUASH/usr/bin/glaciem-miner"                                          "$WORK/glaciem-miner"
cp "$SQUASH/usr/share/icons/hicolor/256x256/apps/glaciem-miner.png"         "$WORK/glaciem-miner.png"
cp "$REPO_ROOT/flatpak/com.hughson.glaciem.desktop"                         "$WORK/com.hughson.glaciem.desktop"
cp "$REPO_ROOT/flatpak/com.hughson.glaciem.metainfo.xml"                    "$WORK/com.hughson.glaciem.metainfo.xml"
cp "$REPO_ROOT/flatpak/com.hughson.glaciem.yaml"                            "$WORK/com.hughson.glaciem.yaml"

# Stage ONLY the libs that the runtime doesn't already provide. The
# org.kde.Platform//6.10 runtime (built on org.freedesktop.Platform//25.08)
# already has glib, curl, openssl, libpng, libxkbcommon, libfontconfig,
# libssh2, etc. — bundling our own versions of those triggers glibc symbol
# mismatches (our build is on Ubuntu 26.04 with glibc 2.43, runtime ships an
# older glibc). The Monero-specific libs aren't in the runtime though, so
# those still need to ride along: Boost, libsodium, libunbound, libhidapi.
STAGE=$(mktemp -d)
shopt -s nullglob
for lib in "$SQUASH/usr/lib"/*.so*; do
    name="$(basename "$lib")"
    case "$name" in
        libboost_*|libsodium.so*|libunbound.so*|libhidapi*.so*|libevent*.so*)
            cp -a "$lib" "$STAGE/"
            ;;
    esac
done
tar -C "$STAGE" -cf "$WORK/vendor-libs.tar" .
rm -rf "$STAGE"
echo "    staged vendor-libs.tar ($(du -sh "$WORK/vendor-libs.tar" | cut -f1), $(tar -tf "$WORK/vendor-libs.tar" | wc -l) entries)"

echo "=== ensure KDE Platform/Sdk 6.10 are present ==="
flatpak --user remote-add --if-not-exists flathub \
    https://flathub.org/repo/flathub.flatpakrepo
flatpak --user install -y --noninteractive flathub \
    org.kde.Platform//6.10 org.kde.Sdk//6.10

echo "=== flatpak-builder ==="
cd "$WORK"
rm -rf build-dir build-repo
flatpak-builder --user --force-clean \
    --repo=build-repo build-dir com.hughson.glaciem.yaml

echo "=== flatpak build-bundle ==="
flatpak build-bundle build-repo Glaciem-Miner-Linux.flatpak com.hughson.glaciem

echo "=== done ==="
ls -lh "$WORK/Glaciem-Miner-Linux.flatpak"
echo
echo "to test locally on glaciem-node:"
echo "  flatpak --user install -y $WORK/Glaciem-Miner-Linux.flatpak"
echo "  flatpak run com.hughson.glaciem"
