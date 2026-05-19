#!/bin/bash
# Standalone macOS build + run of the keygen library check.
# Verifies the keygen sources compile and produce a valid Rime address
# before the library is wired into any app.
set -e
cd "$(dirname "$0")"

REPO=../..
BOOST_INC=$(brew --prefix boost)/include

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
# Rime's own crypto -- C++ sources
CXXSRC="
  $REPO/src/crypto/crypto.cpp
  $REPO/src/common/base58.cpp
  $REPO/src/mnemonics/electrum-words.cpp
  $REPO/contrib/epee/src/wipeable_string.cpp
  $REPO/contrib/epee/src/mlocker.cpp
  $REPO/external/easylogging++/easylogging++.cc
  easylogging_init.cpp
  rime_keygen.cpp
  test_keygen.cpp
"

rm -rf obj && mkdir obj
for f in $CSRC;   do clang   -O2 -std=c11   $INC -c "$f" -o "obj/$(basename "$f").o"; done
for f in $CXXSRC; do clang++ -O2 -std=c++17 $INC -c "$f" -o "obj/$(basename "$f").o"; done
clang++ -O2 obj/*.o -o test_keygen

echo "=== running keygen check ==="
./test_keygen
