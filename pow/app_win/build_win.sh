#!/bin/sh
# build_win.sh -- cross-build "Glaciem Miner.exe" for Windows from macOS/Linux.
#
# A CPU-only Lattice miner with an embedded wallet (Monero wallet_api, built
# from the Rime fork). Produces a single self-contained 64-bit .exe.
#
# Prerequisites (one-time):
#   1) MinGW-w64:   brew install mingw-w64   /   apt install mingw-w64
#   2) Windows dependency stack:
#        make -C ../../contrib/depends HOST=x86_64-w64-mingw32
#   3) Monero wallet libs cross-built for Windows:
#        cd ../.. && mkdir -p build-win && cd build-win
#        cmake -DCMAKE_TOOLCHAIN_FILE=$PWD/../contrib/depends/x86_64-w64-mingw32/share/toolchain.cmake \
#              -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI_DEPS=OFF -DUSE_DEVICE_TREZOR=OFF ..
#        make -j8 wallet_api
set -e
cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
CXX=${CXX:-x86_64-w64-mingw32-g++}

REPO=../..
BW=$REPO/build-win
DEP=$REPO/contrib/depends/x86_64-w64-mingw32
VERSION="$(cat "$REPO/VERSION" | tr -d '[:space:]')"
echo "Glaciem version: $VERSION"

INC="-I$REPO/src -I$REPO/src/crypto -I$REPO/contrib/epee/include -I$REPO/external -I$REPO/external/easylogging++ -I$DEP/include"

echo "[1/3] compiling keygen + embedded-wallet glue (C++)..."
rm -rf winobj && mkdir winobj
$CXX -O2 -std=c++17 $INC -c ../keygen/rime_keygen.cpp -o winobj/rime_keygen.o
$CXX -O2 -std=c++17 $INC -c ../wallet/rime_wallet.cpp -o winobj/rime_wallet.o
$CC  -O2 -c ../wallet/peer_cache.c -o winobj/peer_cache.o

echo "[2/3] compiling rime_miner_win.c + icon resource..."
# -O3 + AVX2/BMI2: the Lattice hash loop (lattice_ref.c is #included here) gets
# auto-vectorised 16-word block mixing that the SSE2 baseline cannot do. AVX2 is
# a safe baseline for any x86 CPU since ~2013/2015 -- for a wider public release,
# switch to runtime CPU dispatch instead of a hard AVX2 requirement.
$CC -O3 -Wall -Wno-unused-parameter -mavx2 -mbmi2 -funroll-loops \
    -DGLACIEM_VERSION="\"$VERSION\"" \
    -c rime_miner_win.c -o winobj/rime_miner_win.o
${WINDRES:-x86_64-w64-mingw32-windres} -i rime.rc -o winobj/rime_res.o

echo "[3/3] linking Glaciem Miner.exe (static -- single self-contained .exe)..."
MONERO_LIBS="
  $BW/lib/libwallet_api.a
  $BW/lib/libwallet.a
  $BW/src/cryptonote_core/libcryptonote_core.a
  $BW/src/cryptonote_basic/libcryptonote_basic.a
  $BW/src/cryptonote_basic/libcryptonote_format_utils_basic.a
  $BW/src/multisig/libmultisig.a
  $BW/src/ringct/libringct.a
  $BW/src/ringct/libringct_basic.a
  $BW/src/checkpoints/libcheckpoints.a
  $BW/src/blockchain_db/libblockchain_db.a
  $BW/src/net/libnet.a
  $BW/src/device/libdevice.a
  $BW/src/device_trezor/libdevice_trezor.a
  $BW/src/hardforks/libhardforks.a
  $BW/src/mnemonics/libmnemonics.a
  $BW/src/rpc/librpc_base.a
  $BW/src/common/libcommon.a
  $BW/src/crypto/libcncrypto.a
  $BW/src/libversion.a
  $BW/contrib/epee/src/libepee.a
  $BW/external/easylogging++/libeasylogging.a
  $BW/external/db_drivers/liblmdb/liblmdb.a
  $BW/external/randomx/librandomx.a
"
DEP_LIBS="
  $DEP/lib/libboost_serialization.a
  $DEP/lib/libboost_thread.a
  $DEP/lib/libboost_chrono.a
  $DEP/lib/libboost_filesystem.a
  $DEP/lib/libboost_program_options.a
  $DEP/lib/libboost_regex.a
  $DEP/lib/libboost_date_time.a
  $DEP/lib/libboost_atomic.a
  $DEP/lib/libboost_container.a
  $DEP/lib/libboost_locale.a
  $DEP/lib/libssl.a
  $DEP/lib/libcrypto.a
  $DEP/lib/libsodium.a
  $DEP/lib/libunbound.a
  $DEP/lib/libzmq.a
  $DEP/lib/libprotobuf.a
  $DEP/lib/libhidapi.a
"
$CXX -O2 -mwindows -static \
  winobj/rime_miner_win.o winobj/rime_keygen.o winobj/rime_wallet.o winobj/peer_cache.o winobj/rime_res.o \
  -Wl,--start-group $MONERO_LIBS $DEP_LIBS -Wl,--end-group \
  -o "Glaciem Miner.exe" \
  -lwinhttp -lws2_32 -lwsock32 -liphlpapi -lcrypt32 -lbcrypt \
  -lgdi32 -luser32 -lkernel32 -ladvapi32 -lshell32 -lole32 -loleaut32 \
  -luuid -lsetupapi -lcfgmgr32 -lntdll -lpsapi -lpthread -lssp
rm -rf winobj

echo "done -- $(pwd)/Glaciem Miner.exe"
