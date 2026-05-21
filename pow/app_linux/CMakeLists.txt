# Glaciem Miner -- Linux Qt6/QML GUI build.
#
# Builds `glaciem-miner` as a Qt6 Quick app. Wires the same C ABI wallet and
# Lattice PoW the Mac/Win/Android apps use, so the UI shows live miner +
# embedded-wallet state on Linux.
#
# Notes for iteration:
#   - The wallet stack (Monero wallet_api + boost + openssl + sodium + ... )
#     is huge. Rather than re-list every static lib, we read the existing
#     wallet_rpc_server link.txt that was produced by the upstream Monero CMake
#     build, parse it for *.a and *.dylib tokens, and link those.
#   - On Mac the build expects ${MONERO_BUILD}/release at REPO/build/release.
#   - On Linux you'd point it at REPO/build/Linux/main/release.

cmake_minimum_required(VERSION 3.16)
project(glaciem_miner LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Find each Qt6 module individually -- the homebrew kegs don't ship the
# umbrella Qt6Config.cmake (only the full `qt` meta-formula does).
foreach(mod IN ITEMS Qt6Core Qt6Gui Qt6Qml Qt6Quick Qt6QuickControls2)
    find_package(${mod} REQUIRED)
endforeach()
find_package(CURL REQUIRED)
# Boost: don't pull in Boost::headers (it adds /opt/homebrew/include, which on
# Mac smuggles qt@5 headers into the include path and breaks the Qt6 build).
# Point directly at boost's own include dir instead.
find_package(Boost REQUIRED CONFIG)
get_target_property(BOOST_INC Boost::headers INTERFACE_INCLUDE_DIRECTORIES)

# ---- locate the Monero wallet build that's already on disk ---------------
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Read the project version from the repo-root VERSION file (single source of
# truth shared with the Mac/Win/Android builds).
file(READ "${REPO_ROOT}/VERSION" GLACIEM_VERSION)
string(STRIP "${GLACIEM_VERSION}" GLACIEM_VERSION)
message(STATUS "Glaciem version: ${GLACIEM_VERSION}")
if(NOT MONERO_BUILD)
    if(EXISTS "${REPO_ROOT}/build/release/lib/libwallet_api.a")
        set(MONERO_BUILD "${REPO_ROOT}/build/release")
    elseif(EXISTS "${REPO_ROOT}/build/Linux/main/release/lib/libwallet_api.a")
        set(MONERO_BUILD "${REPO_ROOT}/build/Linux/main/release")
    else()
        message(FATAL_ERROR "Cannot find a built Monero wallet_api. Build it first "
                           "(see ../app/build.sh on Mac, or BUILD_LINUX.md on Linux).")
    endif()
endif()
message(STATUS "Using Monero wallet build: ${MONERO_BUILD}")

# ---- parse the upstream link.txt to get the full wallet link set ----------
set(LINKTXT "${MONERO_BUILD}/src/wallet/CMakeFiles/wallet_rpc_server.dir/link.txt")
if(NOT EXISTS "${LINKTXT}")
    message(FATAL_ERROR "wallet_rpc_server link.txt not found at ${LINKTXT}")
endif()
file(READ "${LINKTXT}" LINKCONTENT)
separate_arguments(LINKTOKENS UNIX_COMMAND "${LINKCONTENT}")
set(WALLET_LIBS)
foreach(tok IN LISTS LINKTOKENS)
    if(tok MATCHES "\\.(a|dylib|so|so\\.[0-9.]+)$")
        if(NOT IS_ABSOLUTE "${tok}")
            get_filename_component(tok "${tok}" ABSOLUTE
                BASE_DIR "${MONERO_BUILD}/src/wallet")
        endif()
        list(APPEND WALLET_LIBS "${tok}")
    endif()
endforeach()

# ---- our sources ----------------------------------------------------------
add_executable(glaciem-miner
    main.cpp
    miner_engine.cpp
    "${REPO_ROOT}/pow/lattice_ref.c"
    "${REPO_ROOT}/pow/wallet/rime_wallet.cpp"
    "${REPO_ROOT}/pow/wallet/peer_cache.c"
    "${REPO_ROOT}/pow/keygen/rime_keygen.cpp"
    resources.qrc
)
target_compile_definitions(glaciem-miner PRIVATE
    LATTICE_NO_MAIN
    GLACIEM_VERSION="${GLACIEM_VERSION}"
)
target_include_directories(glaciem-miner PRIVATE
    "${REPO_ROOT}/pow/wallet"
    "${REPO_ROOT}/pow/keygen"
    "${REPO_ROOT}/src"
    "${REPO_ROOT}/contrib/epee/include"
    "${REPO_ROOT}/external"
    "${REPO_ROOT}/external/easylogging++"
)
# Header-only deps the Monero crypto stack pulls in (boost / sodium / openssl).
# On Mac we use the keg-specific prefixes -- using /opt/homebrew/include for
# all of them would also drag in qt@5 headers and break the Qt6 build.
if(APPLE)
    target_include_directories(glaciem-miner PRIVATE
        "/opt/homebrew/opt/boost/include"
        "/opt/homebrew/opt/libsodium/include"
        "/opt/homebrew/opt/openssl@3/include"
    )
else()
    target_include_directories(glaciem-miner PRIVATE ${BOOST_INC})
endif()
# the Lattice hot loop wants O3 + auto-vectorization
set_source_files_properties("${REPO_ROOT}/pow/lattice_ref.c"
    PROPERTIES COMPILE_OPTIONS "-O3;-funroll-loops")

target_link_libraries(glaciem-miner PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    CURL::libcurl
    "${MONERO_BUILD}/lib/libwallet_api.a"
    ${WALLET_LIBS}
)
if(APPLE)
    target_link_libraries(glaciem-miner PRIVATE
        "-framework IOKit" "-framework Security" "-framework CoreFoundation")
endif()
