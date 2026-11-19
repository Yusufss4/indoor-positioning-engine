# The JNI library of the Android module: the engine of this checkout (ipe::core, built here as part of this
# build, so only the library and with this toolchain's flags) behind a thin bridge.
cmake_minimum_required(VERSION 3.20)
project(ipe_jni LANGUAGES CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ${CMAKE_CURRENT_BINARY_DIR}/ipe)

add_library(ipe_jni SHARED ipe_jni.cpp)
target_compile_features(ipe_jni PRIVATE cxx_std_20)
target_link_libraries(ipe_jni PRIVATE ipe::core)
# Devices with 16 KB memory pages (Android 15 and later) need libraries aligned for them.
target_link_options(ipe_jni PRIVATE -Wl,-z,max-page-size=16384)
