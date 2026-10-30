cmake_minimum_required(VERSION 3.20)
project(indoor_positioning_engine VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(EMSCRIPTEN)
  set(_ipe_native OFF)
else()
  set(_ipe_native ON)
endif()

option(IPE_BUILD_TOOLS "Build command line tools (sim, replay, regress)" ${_ipe_native})
option(IPE_BUILD_TESTS "Build unit and regression tests" ${_ipe_native})
option(IPE_BUILD_WASM "Build the WebAssembly demo module" ${EMSCRIPTEN})
option(IPE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(IPE_SANITIZE "" CACHE STRING "Comma separated sanitizers, e.g. address,undefined")

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

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
add_subdirectory(sim)
add_subdirectory(eval)

# Map import/export is a native tooling concern; the wasm demo reads venue JSON directly.
if(IPE_BUILD_TOOLS OR IPE_BUILD_TESTS)
  add_subdirectory(geo)
endif()

if(IPE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(IPE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(IPE_BUILD_WASM)
  add_subdirectory(web)
endif()
