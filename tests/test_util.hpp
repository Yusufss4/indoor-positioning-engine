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

}  // namespace ipe::test
