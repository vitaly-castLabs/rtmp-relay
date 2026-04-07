cmake_minimum_required(VERSION 3.14)

set(CMAKE_CXX_COMPILER_NAMES clang++ g++ icpc c++ cxx)
project(rtmp_relay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_path(ASIO_INCLUDE_DIR NAMES asio.hpp REQUIRED)
pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)

add_library(asio INTERFACE)
target_include_directories(asio SYSTEM INTERFACE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
target_link_libraries(asio INTERFACE Threads::Threads)

add_executable(rtmp_relay
    src/main.cpp
    src/rtmp_relay.cpp
)

target_include_directories(rtmp_relay PRIVATE src)
target_compile_options(rtmp_relay PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(
    rtmp_relay
    PRIVATE
        asio
        Threads::Threads
        PkgConfig::LIBAVFORMAT
        PkgConfig::LIBAVCODEC
        PkgConfig::LIBAVUTIL
)
