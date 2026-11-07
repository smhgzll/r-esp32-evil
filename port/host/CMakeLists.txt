# Host simulator: the board's port layer as a native 32-bit Linux binary.
#
#   cmake -S port/host -B build/host -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build/host -j
#
# 32-bit like the decomp's own Linux build (the game's layout assumes 32-bit
# pointers, which is also what the ESP32-S3 has). Needs only g++-multilib /
# glibc-devel.i686 - no SDL, no ffmpeg, no GL.
cmake_minimum_required(VERSION 3.16)
project(re1_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(RE1_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include(${RE1_ROOT}/port/sources.cmake)

option(RE1_HOST_SMALL_MEM "Use the board's memory budget (RE1_SMALL_MEM)" ON)
set(RE1_HOST_FB_W 160 CACHE STRING "Framebuffer width")
set(RE1_HOST_FB_H 120 CACHE STRING "Framebuffer height")

add_executable(re1_host ${RE1_DECOMP_SOURCES} ${RE1_PORT_SOURCES}
               ${CMAKE_CURRENT_SOURCE_DIR}/main_host.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/heaptrace.cpp)
option(RE1_HOST_HEAPTRACE "Track live heap per call site (RE1_HEAPTRACE=1)" OFF)
if(RE1_HOST_HEAPTRACE)
    target_compile_definitions(re1_host PRIVATE RE1_HOST_HEAPTRACE)
endif()
target_include_directories(re1_host PRIVATE ${RE1_INCLUDE_DIRS})
target_compile_definitions(re1_host PRIVATE
    RE1_TARGET_NAME="host"
    RE1_FB_W=${RE1_HOST_FB_W} RE1_FB_H=${RE1_HOST_FB_H}
    RE1_REMOTE_AUDIO
    RE1_NO_HD_TEXTURES
    RE1_TMD_MAX_TRIS_COLLECT=3072
    RE1_BG_ON_DEMAND
    RE1_PORT_VIDEO_RV1
    RE1_PORT_TEXTURE_BGR555
    RE1_PORT_TEXT_SCALE
    RE1_PORT_TEXTURE_INDEXED
    RE1_PORT_VRAM16
    RE1_PORT_EXTRAS
    RE1_PORT_BENCH
    RE1_PORT_PROFILE
    RE1_PORT_TEXTURE_PAL8)
# RE1_PORT_FAST: same pixels, less work. OFF builds the plain code paths, to
# compare captures against (see docs/GUIDE.md, host simulator).
option(RE1_HOST_FAST "Build with RE1_PORT_FAST" ON)
if(RE1_HOST_FAST)
    target_compile_definitions(re1_host PRIVATE RE1_PORT_FAST)
endif()
if(RE1_HOST_SMALL_MEM)
    target_compile_definitions(re1_host PRIVATE RE1_SMALL_MEM)
endif()
target_compile_options(re1_host PRIVATE -m32 ${RE1_COMMON_OPTIONS})
target_link_options(re1_host PRIVATE -m32)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(re1_host PRIVATE Threads::Threads m)
