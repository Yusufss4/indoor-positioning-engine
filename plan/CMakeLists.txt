add_library(ipe_plan STATIC src/plan.cpp)
add_library(ipe::plan ALIAS ipe_plan)

target_include_directories(ipe_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipe_plan PUBLIC ipe_core)
