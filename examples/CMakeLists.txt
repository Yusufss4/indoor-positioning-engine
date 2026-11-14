# Programs that use the engine as a library. They link ipe::core only (ipe_options is this repository's
# warning flags, not a library).
add_executable(ipe-minimal minimal.cpp)
target_link_libraries(ipe-minimal PRIVATE ipe::core ipe_options)
