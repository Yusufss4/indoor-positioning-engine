#pragma once

#include <string>

#include "ipe/venue.hpp"

namespace ipe::test {

inline std::string data_path(const std::string& rel) { return std::string(IPE_DATA_DIR) + "/" + rel; }

/// 20 x 10 m box split by a wall at x = 10 with a 1 m door at y in [4.5, 5.5].
inline Venue two_room_venue() {
  return parse_venue(R"({
    "name": "two_rooms",
    "bounds": [0, 0, 20, 10],
    "wall_attenuation_db": 4.0,
    "walls": [[0,0,20,0],[20,0,20,10],[20,10,0,10],[0,10,0,0],[10,0,10,4.5],[10,5.5,10,10]],
    "beacons": [
      {"id": "A", "x": 2,  "y": 2},
      {"id": "B", "x": 8,  "y": 8},
      {"id": "C", "x": 12, "y": 2},
      {"id": "D", "x": 18, "y": 8}
    ]
  })");
}

/// two_room_venue() with its two rooms named: "west" (index 0) and "east" (index 1).
inline Venue two_room_venue_with_rooms() {
  const Venue v = two_room_venue();
  std::vector<Room> rooms = {{"west", "West room", 0, {{0, 0}, {10, 0}, {10, 10}, {0, 10}}},
                             {"east", "East room", 0, {{10, 0}, {20, 0}, {20, 10}, {10, 10}}}};
  return Venue(v.name(), v.bounds(), v.levels(), v.beacons(), v.connectors(), v.wall_attenuation_db(),
               v.floor_attenuation_db(), v.georef(), v.footprint(), std::move(rooms));
}

/// Two copies of the two-room box stacked 4 m apart, joined by a stairwell in
/// the south-west corner and a lift in the north-east corner.
inline Venue two_level_venue() {
  return parse_venue(R"({
    "name": "two_levels",
    "bounds": [0, 0, 20, 10],
    "wall_attenuation_db": 4.0,
    "floor_attenuation_db": 15.0,
    "georef": {"origin_lat": 41.0, "origin_lon": 29.0, "rotation_deg": 0},
    "levels": [
      {"ordinal": 1, "name": "First", "elevation_m": 4.0,
       "walls": [[0,0,20,0],[20,0,20,10],[20,10,0,10],[0,10,0,0],[10,0,10,4.5],[10,5.5,10,10]]},
      {"ordinal": 0, "name": "Ground", "elevation_m": 0.0,
       "walls": [[0,0,20,0],[20,0,20,10],[20,10,0,10],[0,10,0,0],[10,0,10,4.5],[10,5.5,10,10]]}
    ],
    "connectors": [
      {"id": "stairs", "kind": "stairs", "levels": [0, 1], "area": [[0.5,0.5],[4,0.5],[4,2.5],[0.5,2.5]]},
      {"id": "lift", "kind": "elevator", "levels": [0, 1], "area": [[18,8],[19.5,8],[19.5,9.5],[18,9.5]]}
    ],
    "beacons": [
      {"id": "A0", "x": 2,  "y": 2, "level": 0},
      {"id": "B0", "x": 8,  "y": 8, "level": 0},
      {"id": "C0", "x": 12, "y": 2, "level": 0},
      {"id": "D0", "x": 18, "y": 8, "level": 0},
      {"id": "A1", "x": 3,  "y": 7, "level": 1},
      {"id": "B1", "x": 7,  "y": 3, "level": 1},
      {"id": "C1", "x": 13, "y": 7, "level": 1},
      {"id": "D1", "x": 17, "y": 3, "level": 1}
    ]
  })");
}

/// The 20 x 10 m box above without its north-east quarter: 150 of 200 m^2.
inline Polygon l_footprint() { return {{0, 0}, {20, 0}, {20, 5}, {10, 5}, {10, 10}, {0, 10}}; }

inline Venue with_footprint(const Venue& v, Polygon footprint) {
  return Venue(v.name(), v.bounds(), v.levels(), v.beacons(), v.connectors(), v.wall_attenuation_db(),
               v.floor_attenuation_db(), v.georef(), std::move(footprint));
}

}  // namespace ipe::test
