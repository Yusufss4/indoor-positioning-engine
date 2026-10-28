add_library(ipe_sim STATIC src/simulator.cpp)
add_library(ipe::sim ALIAS ipe_sim)

target_include_directories(ipe_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipe_sim
  PUBLIC ipe_core
  PRIVATE nlohmann_json::nlohmann_json)
