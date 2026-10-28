#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ipe/geometry.hpp"
#include "ipe/georef.hpp"

namespace ipe::geo::detail {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

/// Merges overlapping or touching collinear segments, so that the shared edge
/// of two adjacent rooms becomes one wall instead of two.
std::vector<Segment> merge_collinear(const std::vector<Segment>& segments, double tol = 0.02);

/// Removes the parts of each wall covered by an opening. An opening covers a
/// wall if all its vertices lie within `tol` of the wall's line.
std::vector<Segment> cut_openings(const std::vector<Segment>& walls, const std::vector<std::vector<Vec2>>& openings,
                                  double tol);

/// If `ring` (closing vertex optional) is a rectangle no thicker than
/// `max_thickness`, returns its centre line.
std::optional<Segment> thin_rect_centerline(const std::vector<Vec2>& ring, double max_thickness);

/// Closed ring of the rectangle of width `thickness` around `s`.
std::vector<Vec2> wall_rect(const Segment& s, double thickness);

double round_mm(double v);
/// Rounds degrees to 1e-9 (about 0.1 mm), which keeps exported files short.
double round_deg(double v);

/// GeoJSON position [lon, lat] for a local point.
ojson position(const GeoRef& g, Vec2 p);
/// Closed GeoJSON polygon ring for local points (closing vertex added if missing).
ojson polygon(const GeoRef& g, const std::vector<Vec2>& ring);

/// One feature per line, which keeps diffs of committed exports readable.
std::string feature_collection(const std::vector<ojson>& features);

std::string read_file(const std::string& path);
void write_file(const std::string& path, const std::string& text);

}  // namespace ipe::geo::detail
