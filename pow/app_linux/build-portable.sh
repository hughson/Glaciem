#!/bin/bash
# Build a portable Linux miner — works on any x86_64 CPU from Haswell (2013)
# onward. Use this for AppImage / Flatpak / .deb release builds. For local
# dev iteration where the binary only needs to run on the build host, use
# the simpler build.sh instead.
#
# WHY THIS SCRIPT EXISTS
# ----------------------
# Monero's CMakeLists.txt defaults ARCH=native, which makes -march=native and
# bakes in whatever instructions the build host supports. On glaciem-node
# (AMD EPYC Genoa) that means AVX-512 + VAES. The resulting binary SIGILLs at
# startup on any CPU older than Skylake-X (Intel) or Zen 4 (AMD) — ~30% of
# desktops in 2026, including every laptop made before 2018.
#
# v1.1.11 and v1.1.14 shipped this way and broke for those users. v1.1.15
# fixed it by hand-passing -DARCH=x86-64-v3, but that flag wasn't checked in
# anywhere — pure muscle memory. This script makes the fix permanent.
#
# A secondary Monero CMake quirk: setting ARCH=x86-64-v3 (anything that isn't
# "x86_64", "x86-64", or "amd64") overrides ARCH_ID, which breaks the JIT
# detection in external/randomx/CMakeLists.txt and produces a librandomx.a
# with undefined references to JitCompilerX86::*. Workaround: pass ARCH_ID
# explicitly so it stays "x86_64" while ARCH=x86-64-v3 controls -march.
#
# WHAT GETS BUILT
# ---------------
# Full Linux release chain in /root/glaciem/build/release/ (or wherever this
# repo lives):
#   - libwallet_api.a + transitive deps (Monero wallet API)
#   - libdaemonizer.a, libepee_readline.a (linker requirements)
#   - rimed (daemon binary, /bin/rimed)
#   - rime-wallet-rpc (wallet RPC binary, /bin/rime-wallet-rpc)
#   - glaciem-miner (Qt6 desktop app, pow/app_linux/build/glaciem-miner)
#
# Then sanity-checks the final glaciem-miner binary for AVX-512 or VAES
# instructions and refuses to declare success if any are present.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
JOBS="${JOBS:-2}"   # glaciem-node has 3.7 GB RAM, -j2 fits in memory.
                    # Override for beefier hosts: JOBS=8 bash build-portable.sh.

echo "=== [1/5] Configure Monero build (ARCH_ID=x86_64, ARCH=x86-64-v3) ==="
# x86_64-v3 covers Haswell (2013+) and Zen (2017+), captures ~95% of installed
# desktop CPUs in 2026. If you ever want max compatibility, drop to x86-64-v2
# (Nehalem/Bulldozer baseline, ~99%) at the cost of losing AVX2 in hot loops.
# x86-64-v4 is AVX-512 territory — don't use it for release builds.
mkdir -p "$REPO/build/release"
cd "$REPO/build/release"
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DARCH_ID=x86_64 \
    -DARCH=x86-64-v3 \
    -DBUILD_GUI_DEPS=ON \
    -DUSE_DEVICE_TREZOR=OFF \
    -DBUILD_TESTS=OFF \
    "$REPO"

echo ""
echo "=== [2/5] Build wallet_api + daemon deps (-j$JOBS) ==="
# These are the libs glaciem-miner links against, plus the daemon target
# which restores the rimed binary on disk if it was wiped.
make -j"$JOBS" wallet_api daemonizer epee_readline daemon wallet_rpc_server

echo ""
echo "=== [3/5] Build glaciem-miner (Qt6 app) ==="
cd "$REPO/pow/app_linux"
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j"$JOBS"

echo ""
echo "=== [4/5] Regression check: confirm no AVX-512 / VAES in final binary ==="
BIN="$REPO/pow/app_linux/build/glaciem-miner"
BAD_INSTRUCTIONS=$(objdump -d "$BIN" 2>/dev/null | grep -oE '\b(vpternlog|vpermb|vpermt2|vpermi2|kmov[a-z]+|vplzcntq|vpdpbusd|vaesenc[[:space:]]+[^,]+ymm|vaesenc[[:space:]]+[^,]+zmm|vaesdec[[:space:]]+[^,]+ymm|vaesdec[[:space:]]+[^,]+zmm)\b' | sort -u || true)
if [ -n "$BAD_INSTRUCTIONS" ]; then
    echo "FATAL: glaciem-miner contains instructions that won't run on x86-64-v3 CPUs:" >&2
    echo "$BAD_INSTRUCTIONS" >&2
    echo "Investigate which dependency was compiled without the ARCH flags." >&2
    exit 1
fi
echo "    ✓ no AVX-512, no VAES — binary is portable to any Haswell+ CPU"

echo ""
echo "=== [5/5] Verify final binary at expected path ==="
ls -lh "$BIN"
file "$BIN" | head -1
echo ""
echo "=== done ==="
echo "Next step: bash /root/rebuild-appimage.sh    (packages binary as AppImage)"
echo "Then:     bash $REPO/flatpak/build-flatpak.sh   (packages as Flatpak bundle)"
