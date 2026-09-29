# Builds the shared native payload libvdhs.so (Track R + Track Z).
#
# vdhs_mark.c    - the canonical xrMarkApiClass(0x3b) reach-through (v9)
# vdhs_payload.c - Track Z Mono embedding loader (exports vdhs_payload_main)
# vdhs_layer.c   - compat layer: inline hooks on VD's bundled libopenxr_loader.so
# vdhs_pt.c      - zero-copy alpha hole in the VR projection swapchain (pt_hole)
#
# Built by zygisk/build.ps1 with the NDK cmake/ninja from AGENTS.md.
cmake_minimum_required(VERSION 3.22)
project(vdhs_native C ASM)

# ShadowHook 2.0.1, vendored source, static (see third_party/shadowhook-2.0.1/VENDOR.txt).
set(SH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/shadowhook-2.0.1/cpp)
file(GLOB SH_SRC
    ${SH_DIR}/*.c
    ${SH_DIR}/arch/arm64/*.c
    ${SH_DIR}/arch/arm64/*.S
    ${SH_DIR}/common/*.c
    ${SH_DIR}/third_party/*/*.c)
list(FILTER SH_SRC EXCLUDE REGEX ".*/sh_jni\\.c$")   # no Java API, and it defines JNI_OnLoad
add_library(shadowhook_static STATIC ${SH_SRC})
set_target_properties(shadowhook_static PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(shadowhook_static PRIVATE VDHS_SHADOWHOOK_NO_LINKER_MONITOR)
target_compile_options(shadowhook_static PRIVATE -Os -fPIC -ffunction-sections -fdata-sections -w)
target_include_directories(shadowhook_static
    PRIVATE ${SH_DIR} ${SH_DIR}/arch/arm64 ${SH_DIR}/common
            ${SH_DIR}/third_party/xdl ${SH_DIR}/third_party/bsd ${SH_DIR}/third_party/lss
    PUBLIC  ${SH_DIR}/include)

add_library(vdhs SHARED vdhs_mark.c vdhs_payload.c vdhs_layer.c vdhs_hand.c vdhs_pt.c)
set_target_properties(vdhs PROPERTIES
    OUTPUT_NAME "vdhs"
    PREFIX "lib"
    C_STANDARD 11)
target_compile_options(vdhs PRIVATE -O2 -fPIC -Wall -Wextra -Wno-unused-parameter)
target_link_options(vdhs PRIVATE "-Wl,-soname,libvdhs.so" "-Wl,--exclude-libs,ALL" "-Wl,--gc-sections"
    "-Wl,-z,max-page-size=16384")
target_link_libraries(vdhs PRIVATE shadowhook_static log dl)
