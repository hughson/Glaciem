#!/bin/bash
# Glaciem Miner -- Linux Qt6/QML build (also runs on Mac for iteration).
#
# Linux one-time deps (Ubuntu/Debian):
#   sudo apt update
#   sudo apt install -y build-essential cmake \
#        qt6-base-dev qt6-declarative-dev qt6-tools-dev \
#        libcurl4-openssl-dev
#
# Mac one-time deps:
#   brew install qtbase qtdeclarative
#   # ensure the Monero wallet libs are built (../app/build.sh does this)
#
# Build:  ./build.sh
# Run:    ./build/glaciem-miner
set -e
cd "$(dirname "$0")"

mkdir -p build && cd build

# Mac: keg-only Qt6 modules need explicit prefix paths so find_package(Qt6Core)
# etc. can locate each module's *Config.cmake.
CMAKE_EXTRA=()
if [[ "$OSTYPE" == "darwin"* ]]; then
    QTBASE="$(brew --prefix qtbase 2>/dev/null || true)"
    QTDECL="$(brew --prefix qtdeclarative 2>/dev/null || true)"
    if [ -n "$QTBASE" ] && [ -n "$QTDECL" ]; then
        CMAKE_EXTRA+=(-DCMAKE_PREFIX_PATH="$QTBASE;$QTDECL")
    fi
fi

cmake -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA[@]}" ..
cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

echo
echo "built: $(pwd)/glaciem-miner"
echo "run:   $(pwd)/glaciem-miner"
