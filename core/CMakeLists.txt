add_library(ipe_core STATIC
  src/baro.cpp
  src/beacon_integrity.cpp
  src/config.cpp
  src/engine.cpp
  src/fingerprint_map.cpp
  src/floor_classifier.cpp
  src/filter/estimate.cpp
  src/filter/fingerprint.cpp
  src/filter/grid_filter.cpp
  src/filter/motion.cpp
  src/filter/position_fix.cpp
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

# `ipe::core` in this build and, installed, after find_package(ipe).
set_target_properties(ipe_core PROPERTIES EXPORT_NAME core)
target_compile_features(ipe_core PUBLIC cxx_std_20)
target_include_directories(ipe_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Both are build-time only: the project's flags, and a header-only library no public header includes.
target_link_libraries(ipe_core PRIVATE $<BUILD_INTERFACE:ipe_options>
                                       $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)
