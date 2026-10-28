#pragma once

#include "ipe/geometry.hpp"

namespace ipe {

struct LatLon {
  double lat = 0.0;  ///< Degrees, WGS84.
  double lon = 0.0;
};

/// Anchors the venue's local metric frame on the globe. The local frame is a
/// tangent plane at the origin; `rotation_deg` is the counter-clockwise angle
/// from true east to the local +x axis. Over a building (< a few km) the
/// tangent-plane error is far below the accuracy of anything indoors.
struct GeoRef {
  double origin_lat = 0.0;
  double origin_lon = 0.0;
  double rotation_deg = 0.0;

  [[nodiscard]] LatLon to_wgs84(Vec2 local) const;
  [[nodiscard]] Vec2 to_local(LatLon ll) const;
};

}  // namespace ipe
