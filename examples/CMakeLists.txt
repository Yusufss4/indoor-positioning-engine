# Programs that use the engine as a library. They link ipe::core only.
add_executable(ipe-minimal minimal.cpp)
target_link_libraries(ipe-minimal PRIVATE ipe::core)
