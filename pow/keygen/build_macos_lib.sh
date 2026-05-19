#!/bin/bash
# Build librimekeygen.a for macOS (arm64) -- the keygen library, no test main.
# Linked into Rime Miner.app by pow/app/build.sh.
set -e
cd "$(dirname "$0")"

REPO=../..
BOOST_INC=$(brew --prefix boost)/include

# -Ishim provides the boost::mutex shim and a libsodium header stand-in, so the
# keygen needs no linked boost and no libsodium at all.
INC="-Ishim -I$REPO/src -I$REPO/src/crypto -I$REPO/contrib/epee/include -I$REPO/external/easylogging++ -I$BOOST_INC"

# Rime's own crypto -- C sources
CSRC="
  $REPO/src/crypto/crypto-ops.c
  $REPO/src/crypto/crypto-ops-data.c
  $REPO/src/crypto/keccak.c
  $REPO/src/crypto/hash.c
  $REPO/src/crypto/random.c
  $REPO/contrib/epee/src/memwipe.c
"
# Rime's own crypto -- C++ sources, plus the keygen wrapper
CXXSRC="
  $REPO/src/crypto/crypto.cpp
  $REPO/src/common/base58.cpp
  $REPO/src/mnemonics/electrum-words.cpp
  $REPO/contrib/epee/src/wipeable_string.cpp
  $REPO/contrib/epee/src/mlocker.cpp
  $REPO/external/easylogging++/easylogging++.cc
  easylogging_init.cpp
  rime_keygen.cpp
"

rm -rf obj-lib && mkdir obj-lib
for f in $CSRC;   do clang   -O2 -std=c11   $INC -c "$f" -o "obj-lib/$(basename "$f").o"; done
for f in $CXXSRC; do clang++ -O2 -std=c++17 $INC -c "$f" -o "obj-lib/$(basename "$f").o"; done
ar rcs librimekeygen.a obj-lib/*.o
echo "built $(pwd)/librimekeygen.a"
