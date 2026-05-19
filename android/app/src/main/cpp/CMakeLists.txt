cmake_minimum_required(VERSION 3.22.1)
project(rime_jni C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# pow/ holds the canonical Lattice reference; the repo root is its parent.
get_filename_component(POW_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../pow" ABSOLUTE)
get_filename_component(REPO_DIR "${POW_DIR}/.." ABSOLUTE)

# Header-only Boost for the keygen (shared_ptr, optional, crc, algorithm/string).
set(BOOST_INC "")
foreach(p /opt/homebrew/opt/boost/include /usr/local/include /opt/local/include)
  if(EXISTS "${p}/boost/version.hpp")
    set(BOOST_INC "${p}")
  endif()
endforeach()
if(BOOST_INC STREQUAL "")
  message(FATAL_ERROR "Boost headers not found. brew install boost")
endif()

find_library(log-lib log)

# =====================================================================
# librime_jni.so -- the Lattice miner + the keygen (Glaciem's own crypto).
# Unchanged: a self-contained miner .so.
# =====================================================================
set(KEYGEN_SOURCES
    ${REPO_DIR}/src/crypto/crypto-ops.c
    ${REPO_DIR}/src/crypto/crypto-ops-data.c
    ${REPO_DIR}/src/crypto/keccak.c
    ${REPO_DIR}/src/crypto/hash.c
    ${REPO_DIR}/src/crypto/random.c
    ${REPO_DIR}/contrib/epee/src/memwipe.c
    ${REPO_DIR}/src/crypto/crypto.cpp
    ${REPO_DIR}/src/common/base58.cpp
    ${REPO_DIR}/src/mnemonics/electrum-words.cpp
    ${REPO_DIR}/contrib/epee/src/wipeable_string.cpp
    ${REPO_DIR}/contrib/epee/src/mlocker.cpp
    ${REPO_DIR}/external/easylogging++/easylogging++.cc
    ${POW_DIR}/keygen/easylogging_init.cpp
    ${POW_DIR}/keygen/rime_keygen.cpp
)
add_library(rime_jni SHARED
    lattice_jni.c ${POW_DIR}/lattice_ref.c ${KEYGEN_SOURCES})
target_include_directories(rime_jni PRIVATE
    ${POW_DIR} ${POW_DIR}/keygen ${POW_DIR}/keygen/shim
    ${REPO_DIR}/src ${REPO_DIR}/src/crypto
    ${REPO_DIR}/contrib/epee/include ${REPO_DIR}/external/easylogging++
    ${BOOST_INC})
# LATTICE_NO_MAIN drops lattice_ref.c's self-test main (it is a library here).
target_compile_definitions(rime_jni PRIVATE LATTICE_NO_MAIN)
target_compile_options(rime_jni PRIVATE
    -O3 -fno-strict-aliasing -funroll-loops
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>)
target_link_libraries(rime_jni ${log-lib})

# =====================================================================
# librime_wallet.so -- the embedded wallet (Monero wallet_api).
# A SEPARATE .so so it doesn't collide with the keygen's crypto symbols.
# Links the libwallet_api + dependency stack cross-compiled into
# build-android/ and contrib/depends/aarch64-linux-android/.
# =====================================================================
set(MON  "${REPO_DIR}/build-android")
set(DEP  "${REPO_DIR}/contrib/depends/aarch64-linux-android")

if(EXISTS "${MON}/rime-libs/libwallet_api.a")
  add_library(rime_wallet SHARED
      wallet_jni.cpp
      randomx_stub.c
      ${POW_DIR}/wallet/rime_wallet.cpp)

  target_include_directories(rime_wallet PRIVATE
      ${POW_DIR}/wallet
      ${REPO_DIR}/src
      ${REPO_DIR}/contrib/epee/include
      ${REPO_DIR}/external/easylogging++
      ${DEP}/include)

  target_compile_options(rime_wallet PRIVATE
      -O2 -fexceptions -frtti)

  # Monero static libs -- re-archived with llvm-ar into rime-libs/ (the
  # cross-build's own .a files were created empty by the host's BSD ar).
  # Circular dependencies -> linked as one group below.
  set(MONERO_LIBS
      ${MON}/rime-libs/libwallet_api.a
      ${MON}/rime-libs/libwallet.a
      ${MON}/rime-libs/libcryptonote_core.a
      ${MON}/rime-libs/libcryptonote_basic.a
      ${MON}/rime-libs/libcryptonote_format_utils_basic.a
      ${MON}/rime-libs/libmultisig.a
      ${MON}/rime-libs/libringct.a
      ${MON}/rime-libs/libringct_basic.a
      ${MON}/rime-libs/libcheckpoints.a
      ${MON}/rime-libs/libblockchain_db.a
      ${MON}/rime-libs/libnet.a
      ${MON}/rime-libs/libdevice.a
      ${MON}/rime-libs/libdevice_trezor.a
      ${MON}/rime-libs/libhardforks.a
      ${MON}/rime-libs/libmnemonics.a
      ${MON}/rime-libs/librpc_base.a
      ${MON}/rime-libs/libcommon.a
      ${MON}/rime-libs/libcncrypto.a
      ${MON}/rime-libs/libversion.a
      ${MON}/rime-libs/libepee.a
      ${MON}/rime-libs/libeasylogging.a
      ${MON}/rime-libs/liblmdb.a)

  # external dependency stack (depends/)
  set(DEP_LIBS
      ${DEP}/lib/libboost_serialization.a
      ${DEP}/lib/libboost_thread.a
      ${DEP}/lib/libboost_chrono.a
      ${DEP}/lib/libboost_filesystem.a
      ${DEP}/lib/libboost_program_options.a
      ${DEP}/lib/libboost_regex.a
      ${DEP}/lib/libboost_date_time.a
      ${DEP}/lib/libboost_atomic.a
      ${DEP}/lib/libboost_container.a
      ${DEP}/lib/libssl.a
      ${DEP}/lib/libcrypto.a
      ${DEP}/lib/libsodium.a
      ${DEP}/lib/libunbound.a
      ${DEP}/lib/libzmq.a
      ${DEP}/lib/libprotobuf.a)

  target_link_libraries(rime_wallet
      -Wl,--start-group ${MONERO_LIBS} ${DEP_LIBS} -Wl,--end-group
      ${log-lib})
else()
  message(WARNING "build-android/ libwallet_api.a not found -- skipping rime_wallet")
endif()
