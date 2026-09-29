# Zygisk module library. Output must be named arm64-v8a.so and live in the
# module's zygisk/ directory (Magisk convention).
cmake_minimum_required(VERSION 3.22)
project(vdhs_zygisk LANGUAGES CXX)

add_library(vdhs_zygisk SHARED module.cpp)
set_target_properties(vdhs_zygisk PROPERTIES
    OUTPUT_NAME "arm64-v8a"
    PREFIX "")
target_compile_options(vdhs_zygisk PRIVATE
    -O2 -fPIC -Wall -Wextra -Wno-unused-parameter
    -fno-exceptions -fno-rtti)
target_link_options(vdhs_zygisk PRIVATE "-s")
# APP_STL=none equivalent: no C++ runtime.
target_link_libraries(vdhs_zygisk PRIVATE log dl)
