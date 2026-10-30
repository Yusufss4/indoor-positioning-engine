add_library(ipe_core STATIC
  src/baro.cpp
  src/engine.cpp
  src/ekf.cpp
  src/geometry.cpp
  src/georef.cpp
  src/particle_filter.cpp
  src/pdr.cpp
  src/rooms.cpp
  src/rssi_model.cpp
  src/trace.cpp
  src/venue.cpp
  src/wcl.cpp)
add_library(ipe::core ALIAS ipe_core)

target_include_directories(ipe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipe_core
  PUBLIC ipe_options
  PRIVATE nlohmann_json::nlohmann_json)
