#pragma once

#include <span>
#include <vector>

#include "ipe/geometry.hpp"
#include "ipe/venue.hpp"

/// Beacon placement analysis: how well a beacon layout can localise a phone,
/// from the venue map and the engine's RSSI model alone (no simulation).
/// Native tooling, like ipe_geo; the engine does not link it.
namespace ipe::plan {

struct AnalysisConfig {
  double grid_m = 1.0;           ///< Spacing of the evaluated points.
  double sigma_db = 5.0;         ///< RSSI spread per packet, as RssiModelConfig::sigma_db.
  double min_rssi_dbm = -85.0;   ///< Beacons expected weaker than this are not heard.
  double offset_sigma_db = 8.0;  ///< Prior spread of the unknown device offset; 0 treats it as known.
  double target_m = 2.0;         ///< Summary::share_within_target counts points with crlb_m <= this.
  /// score_path adds up the information heard over the last window_m metres
  /// of the walk (one packet per beacon per metre), as if dead reckoning knew
  /// the displacement exactly; 0 scores every point on its own.
  double window_m = 10.0;
  bool wall_aware = true;  ///< Subtract the map's wall loss when deciding what is heard.
};

/// A position on a level (level index, see Venue::levels()).
struct Location {
  Vec2 pos;
  int level = 0;
};

struct PointScore {
  Location at;
  int heard = 0;  ///< Beacons expected above min_rssi_dbm.
  /// Cramer-Rao bound on the horizontal RMS error from one packet of every
  /// heard beacon, with the device offset as a nuisance parameter. Infinite
  /// where the heard beacons cannot fix a position (e.g. fewer than three
  /// with an unknown offset, or all on one line through the point).
  double crlb_m = 0.0;
  /// Strongest beacon on the point's level minus the strongest on any other
  /// level [dB], both raised to min_rssi_dbm (an unheard level counts as the
  /// threshold). Negative values invite a wrong floor; 0 if nothing is heard.
  double floor_margin_db = 0.0;
};

struct Summary {
  std::size_t points = 0;
  double heard_mean = 0.0;
  double share_heard_below_3 = 0.0;  ///< Points where fewer than three beacons are heard.
  double crlb_p50_m = 0.0;           ///< Infinite if half the points are unbounded.
  double crlb_p95_m = 0.0;
  double share_within_target = 0.0;  ///< Points with crlb_m <= AnalysisConfig::target_m.
  double floor_margin_p05_db = 0.0;  ///< Fifth percentile; meaningful in multi-level venues only.
  double share_floor_margin_negative = 0.0;
};

/// Cell centres of a `spacing` grid over the venue bounds that lie inside the
/// venue (footprint), for every level.
std::vector<Location> venue_grid(const Venue& venue, double spacing);

/// Points every `spacing` metres along consecutive waypoints. Legs that
/// change level (stairs, lifts) are skipped; their end points are kept.
std::vector<Location> sample_path(std::span<const Location> waypoints, double spacing);

std::vector<PointScore> score_points(const Venue& venue, std::span<const Location> points, const AnalysisConfig& cfg);

/// score_points along a walk (consecutive points, e.g. from sample_path),
/// with crlb_m taken over AnalysisConfig::window_m of trailing walk.
std::vector<PointScore> score_path(const Venue& venue, std::span<const Location> path, const AnalysisConfig& cfg);

Summary summarize(std::span<const PointScore> scores, const AnalysisConfig& cfg);

}  // namespace ipe::plan
