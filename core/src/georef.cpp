#include "ipe/georef.hpp"

#include <cmath>

namespace ipe {
namespace {

constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84E2 = 6.69437999014e-3;

/// Metres per radian of latitude and of longitude at `lat_deg` (meridian and
/// prime-vertical radii of curvature).
void radii(double lat_deg, double& m_per_rad_lat, double& m_per_rad_lon) {
  const double phi = deg2rad(lat_deg);
  const double s = std::sin(phi);
  const double w = 1.0 - kWgs84E2 * s * s;
  m_per_rad_lat = kWgs84A * (1.0 - kWgs84E2) / (w * std::sqrt(w));
  m_per_rad_lon = kWgs84A / std::sqrt(w) * std::cos(phi);
}

}  // namespace

LatLon GeoRef::to_wgs84(Vec2 local) const {
  const double th = deg2rad(rotation_deg);
  const double east = local.x * std::cos(th) - local.y * std::sin(th);
  const double north = local.x * std::sin(th) + local.y * std::cos(th);
  double m_lat = 0.0, m_lon = 0.0;
  radii(origin_lat, m_lat, m_lon);
  return {origin_lat + rad2deg(north / m_lat), origin_lon + rad2deg(east / m_lon)};
}

Vec2 GeoRef::to_local(LatLon ll) const {
  double m_lat = 0.0, m_lon = 0.0;
  radii(origin_lat, m_lat, m_lon);
  const double north = deg2rad(ll.lat - origin_lat) * m_lat;
  const double east = deg2rad(ll.lon - origin_lon) * m_lon;
  const double th = deg2rad(rotation_deg);
  return {east * std::cos(th) + north * std::sin(th), -east * std::sin(th) + north * std::cos(th)};
}

}  // namespace ipe
