cmake_minimum_required(VERSION 3.20)
project(indoor_positioning_engine VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Built on its own, or inside another project (add_subdirectory, FetchContent)? Inside another project only
# the library is built, and nothing of that project's setup is touched.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_ipe_top_level ON)
else()
  set(_ipe_top_level OFF)
endif()

if(_ipe_top_level AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(EMSCRIPTEN OR NOT _ipe_top_level)
  set(_ipe_native OFF)
else()
  set(_ipe_native ON)
endif()

option(IPE_BUILD_TOOLS "Build command line tools (sim, replay, regress)" ${_ipe_native})
option(IPE_BUILD_TESTS "Build unit and regression tests" ${_ipe_native})
option(IPE_BUILD_EXAMPLES "Build the example programs" ${_ipe_native})
option(IPE_BUILD_WASM "Build the WebAssembly demo module" ${EMSCRIPTEN})
option(IPE_INSTALL "Install the library, its headers and its CMake package (find_package(ipe))" ${_ipe_native})
option(IPE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(IPE_SANITIZE "" CACHE STRING "Comma separated sanitizers, e.g. address,undefined")

# JSON parsing is internal to the library: no public header includes it. A project that already has the
# target keeps its own copy.
include(FetchContent)
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# The project's own warning and sanitizer flags. Every target of this repository links it; it is not part
# of ipe::core's interface, so a program that uses the library keeps its own flags.
add_library(ipe_options INTERFACE)
if(MSVC)
  target_compile_options(ipe_options INTERFACE /W4)
else()
  target_compile_options(ipe_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                               -Wno-sign-conversion)
  if(IPE_WARNINGS_AS_ERRORS)
    target_compile_options(ipe_options INTERFACE -Werror)
  endif()
endif()
if(IPE_SANITIZE)
  target_compile_options(ipe_options INTERFACE -fsanitize=${IPE_SANITIZE} -fno-omit-frame-pointer
                                               -fno-sanitize-recover=all)
  target_link_options(ipe_options INTERFACE -fsanitize=${IPE_SANITIZE})
endif()

add_subdirectory(core)
if(IPE_BUILD_TOOLS OR IPE_BUILD_TESTS OR IPE_BUILD_WASM)
  add_subdirectory(sim)
  add_subdirectory(eval)
endif()

# Map import/export is native tooling; the wasm demo reads venue JSON directly.
if(IPE_BUILD_TOOLS OR IPE_BUILD_TESTS)
  add_subdirectory(geo)
endif()

if(IPE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(IPE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(IPE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(IPE_BUILD_WASM)
  add_subdirectory(web)
endif()

# cmake --install build --prefix DIR, then in another project:
#   find_package(ipe REQUIRED)
#   target_link_libraries(app PRIVATE ipe::core)
if(IPE_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(_ipe_package_dir ${CMAKE_INSTALL_LIBDIR}/cmake/ipe)
  install(TARGETS ipe_core EXPORT ipeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY core/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT ipeTargets NAMESPACE ipe:: DESTINATION ${_ipe_package_dir})
  configure_package_config_file(cmake/ipeConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/ipeConfig.cmake
                                INSTALL_DESTINATION ${_ipe_package_dir})
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ipeConfigVersion.cmake
                                   COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ipeConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/ipeConfigVersion.cmake
          DESTINATION ${_ipe_package_dir})
  if(IPE_BUILD_TOOLS)
    install(TARGETS ipe-sim ipe-replay ipe-regress ipe-import RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()
