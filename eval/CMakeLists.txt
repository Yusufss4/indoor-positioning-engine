add_library(ipe_eval STATIC src/eval.cpp src/regression.cpp)
add_library(ipe::eval ALIAS ipe_eval)

target_include_directories(ipe_eval PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipe_eval
  PUBLIC ipe_core ipe_sim
  PRIVATE nlohmann_json::nlohmann_json)
