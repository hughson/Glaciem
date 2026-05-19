cmake_minimum_required(VERSION 3.16)
project(rime_miner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

add_executable(rime-miner
    main.cpp
    resources.qrc
)

target_link_libraries(rime-miner PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
)
