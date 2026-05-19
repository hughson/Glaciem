#!/bin/bash
# Generate AppIcon.icns for Rime Miner.app from make_icon.swift.
set -e
cd "$(dirname "$0")"

swift make_icon.swift

rm -rf AppIcon.iconset
mkdir AppIcon.iconset
for s in 16 32 128 256 512; do
  s2=$((s*2))
  sips -z $s  $s  icon_1024.png --out "AppIcon.iconset/icon_${s}x${s}.png"      >/dev/null
  sips -z $s2 $s2 icon_1024.png --out "AppIcon.iconset/icon_${s}x${s}@2x.png"   >/dev/null
done
iconutil -c icns AppIcon.iconset -o AppIcon.icns
rm -rf AppIcon.iconset icon_1024.png
echo "AppIcon.icns created"
