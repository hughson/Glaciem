#!/bin/bash
# Build "Rime Miner.app" -- a SwiftUI mining app, no Xcode required.
set -e
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"

APP="Glaciem Miner.app"
BIN="RimeMiner"

echo "[1/7] building keygen library (Rime's real crypto)..."
../keygen/build_macos_lib.sh

echo "[2/7] compiling Lattice PoW..."
clang -c -O3 -funroll-loops -DLATTICE_NO_MAIN ../lattice_ref.c -o lattice.o

echo "[3/7] compiling miner core (C / Objective-C, CPU-only)..."
clang -c -O2 -I../wallet ../wallet/peer_cache.c -o peer_cache.o
clang -c -fobjc-arc -O2 -I../keygen -I../wallet miner_core.m -o miner_core.o

echo "[4/7] compiling embedded wallet (C ABI over Monero wallet_api)..."
clang++ -c -std=gnu++17 -O2 \
  -I"$REPO/src" -I"$REPO/external" -I"$REPO/contrib/epee/include" \
  ../wallet/rime_wallet.cpp -o rime_wallet.o

echo "[5/7] collecting wallet_api link libraries..."
# Crib the exact macOS wallet link set from the rime-wallet-rpc target CMake
# already configured -- relative .a paths resolved, homebrew .dylib paths kept.
WDIR="$REPO/build/release/src/wallet"
LINKTXT="$WDIR/CMakeFiles/wallet_rpc_server.dir/link.txt"
WALLET_LIBS=()
for tok in $(cat "$LINKTXT"); do
  case "$tok" in
    *.a)     WALLET_LIBS+=("$(python3 -c 'import os,sys;print(os.path.normpath(os.path.join(sys.argv[1],sys.argv[2])))' "$WDIR" "$tok")") ;;
    *.dylib) WALLET_LIBS+=("$tok") ;;
  esac
done

echo "[6/7] compiling SwiftUI app..."
swiftc -O -swift-version 5 -parse-as-library \
  RimeMiner.swift miner_core.o lattice.o rime_wallet.o peer_cache.o \
  "$REPO/build/release/lib/libwallet_api.a" \
  "${WALLET_LIBS[@]}" \
  ../keygen/librimekeygen.a -lc++ \
  -framework IOKit -framework Security -framework CoreFoundation \
  -import-objc-header bridging.h \
  -framework SwiftUI -framework AppKit -framework Foundation \
  -framework CoreImage \
  -o "$BIN"

echo "[7/7] assembling $APP bundle..."
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
mv "$BIN" "$APP/Contents/MacOS/$BIN"
cp Info.plist "$APP/Contents/Info.plist"
[ -f AppIcon.icns ] || ./make_icon.sh
cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"

echo "ad-hoc code signing..."
codesign --force --deep --sign - "$APP" 2>/dev/null || echo "  (codesign skipped)"

rm -f miner_core.o lattice.o rime_wallet.o peer_cache.o

echo "installing to /Applications (so a stale copy can't crash on launch)..."
rm -rf "/Applications/$APP"
ditto "$APP" "/Applications/$APP"

echo "done -- /Applications/$APP"
echo "launch:  open \"/Applications/$APP\""
