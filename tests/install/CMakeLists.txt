# A program outside this repository's build that uses the engine, in the two supported ways:
#   -DIPE_SOURCE_DIR=<checkout>  as part of the program's own build (add_subdirectory / FetchContent)
#   otherwise                    from an installed package (find_package)
# Run by the CTest `package_*` tests through run.cmake.
cmake_minimum_required(VERSION 3.20)
project(ipe_consumer LANGUAGES CXX)

if(IPE_SOURCE_DIR)
  add_subdirectory(${IPE_SOURCE_DIR} ipe)
else()
  find_package(ipe REQUIRED)
endif()

add_executable(consumer ${IPE_EXAMPLE_SOURCE})
target_link_libraries(consumer PRIVATE ipe::core)
