add_library(ipe_core STATIC
  src/baro.cpp
  src/beacon_integrity.cpp
  src/engine.cpp
  src/fingerprint_map.cpp
  src/floor_classifier.cpp
  src/filter/estimate.cpp
  src/filter/fingerprint.cpp
  src/filter/grid_filter.cpp
  src/filter/motion.cpp
  src/filter/pressure.cpp
  src/filter/radio.cpp
  src/filter/recovery.cpp
  src/geometry.cpp
  src/grid_map.cpp
  src/georef.cpp
  src/level_selector.cpp
  src/map_estimate.cpp
  src/reachable_area.cpp
  src/pdr.cpp
  src/rooms.cpp
  src/rssi_model.cpp
  src/trace.cpp
  src/trace_player.cpp
  src/track_smoother.cpp
  src/venue.cpp)
add_library(ipe::core ALIAS ipe_core)

target_include_directories(ipe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ipe_core
  PUBLIC ipe_options
  PRIVATE nlohmann_json::nlohmann_json)
