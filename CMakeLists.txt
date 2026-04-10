cmake_minimum_required(VERSION 3.14)

set(CMAKE_C_COMPILER_NAMES clang gcc icc cc)
set(CMAKE_CXX_COMPILER_NAMES clang++ g++ icpc c++ cxx)
project(rtmp_relay VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENABLE_SANITIZER "none" CACHE STRING "Enable a sanitizer (none, tsan, asan, msan, ubsan)")
set_property(CACHE ENABLE_SANITIZER PROPERTY STRINGS none tsan asan msan ubsan)

if(ENABLE_SANITIZER STREQUAL "tsan")
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
elseif(ENABLE_SANITIZER STREQUAL "asan")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address)
elseif(ENABLE_SANITIZER STREQUAL "msan")
    add_compile_options(-fsanitize=memory -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=memory)
elseif(ENABLE_SANITIZER STREQUAL "ubsan")
    add_compile_options(-fsanitize=undefined -g -O1)
    add_link_options(-fsanitize=undefined)
elseif(NOT ENABLE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "ENABLE_SANITIZER must be 'none', 'tsan', 'asan', 'msan', or 'ubsan'")
endif()

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
    src/transform.cpp
    src/video_param_sets.cpp
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
        ${CMAKE_DL_LIBS}
)

add_library(transform SHARED samples/transform.c)
target_include_directories(transform PRIVATE include)
set_target_properties(transform PROPERTIES PREFIX "" OUTPUT_NAME "transform")
