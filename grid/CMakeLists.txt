add_library(ipe_grid STATIC src/grid_map.cpp src/grid_filter.cpp src/grid_engine.cpp)
add_library(ipe::grid ALIAS ipe_grid)

target_include_directories(ipe_grid PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipe_grid
  PUBLIC ipe_core ipe_eval
  PRIVATE nlohmann_json::nlohmann_json)
