add_library(ipe_geo STATIC src/geo_internal.cpp src/geojson.cpp src/imdf.cpp)
add_library(ipe::geo ALIAS ipe_geo)

target_include_directories(ipe_geo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipe_geo
  PUBLIC ipe_core
  PRIVATE nlohmann_json::nlohmann_json)
