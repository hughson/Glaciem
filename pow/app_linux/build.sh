#!/bin/bash
# Rime Miner -- Linux build (Qt6 / QML).
#
# One-time dependencies (Ubuntu / Debian):
#   sudo apt update
#   sudo apt install -y build-essential cmake \
#        qt6-base-dev qt6-declarative-dev \
#        qml6-module-qtquick qml6-module-qtquick-window
#
# Build:   ./build.sh        ->   build/rime-miner
# Run:     ./build/rime-miner
set -e
cd "$(dirname "$0")"

mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "built: $(pwd)/rime-miner"
echo "run:   $(pwd)/rime-miner"
