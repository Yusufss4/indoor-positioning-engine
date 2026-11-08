#include "ipe/plan.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "ipe/rssi_model.hpp"

namespace ipe::plan {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

/// Nearest-rank percentile; infinities sort to the ends instead of poisoning
/// an interpolation.
double percentile(std::vector<double> values, double p) {
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(values.begin(), values.end());
  const auto n = static_cast<double>(values.size());
  const auto rank = static_cast<std::size_t>(std::clamp(std::ceil(p * n) - 1.0, 0.0, n - 1.0));
  return values[rank];
}

}  // namespace

std::vector<Location> venue_grid(const Venue& venue, double spacing) {
  if (!(spacing > 0.0)) throw std::invalid_argument("grid spacing must be positive");
  const Bounds& b = venue.bounds();
  const auto nx = static_cast<int>(std::floor(b.width() / spacing));
  const auto ny = static_cast<int>(std::floor(b.height() / spacing));
  std::vector<Location> out;
  for (int level = 0; level < venue.level_count(); ++level) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        const Vec2 p{b.min_x + (i + 0.5) * spacing, b.min_y + (j + 0.5) * spacing};
        if (venue.inside(p)) out.push_back({p, level});
      }
    }
  }
  return out;
}

std::vector<Location> sample_path(std::span<const Location> waypoints, double spacing) {
  if (!(spacing > 0.0)) throw std::invalid_argument("path spacing must be positive");
  std::vector<Location> out;
  for (std::size_t i = 0; i < waypoints.size(); ++i) {
    const Location& a = waypoints[i];
    out.push_back(a);
    if (i + 1 == waypoints.size() || waypoints[i + 1].level != a.level) continue;
    const Vec2 d = waypoints[i + 1].pos - a.pos;
    const int n = std::max(1, static_cast<int>(std::ceil(d.norm() / spacing)));
    for (int k = 1; k < n; ++k) out.push_back({a.pos + d * (static_cast<double>(k) / n), a.level});
  }
  return out;
}

namespace {

/// Fisher information about (x, y, device offset) from one packet of every
/// heard beacon, without the offset prior.
struct Info {
  double xx = 0.0, xy = 0.0, yy = 0.0;  // position block
  double xo = 0.0, yo = 0.0;            // position-offset coupling
  double oo = 0.0;

  Info& operator+=(const Info& o) {
    xx += o.xx, xy += o.xy, yy += o.yy, xo += o.xo, yo += o.yo, oo += o.oo;
    return *this;
  }
};

double bound_m(const Info& j, const AnalysisConfig& cfg) {
  double xx = j.xx, xy = j.xy, yy = j.yy;
  if (cfg.offset_sigma_db > 0.0) {
    const double oo = j.oo + 1.0 / (cfg.offset_sigma_db * cfg.offset_sigma_db);
    xx -= j.xo * j.xo / oo;
    xy -= j.xo * j.yo / oo;
    yy -= j.yo * j.yo / oo;
  }
  const double tr = xx + yy;
  const double det = xx * yy - xy * xy;
  return (tr > 0.0 && det > 1e-9 * tr * tr) ? std::sqrt(tr / det) : kInf;
}

std::vector<PointScore> score_with_info(const Venue& venue, std::span<const Location> points, const AnalysisConfig& cfg,
                                        std::vector<Info>& infos) {
  RssiModelConfig model_cfg;
  model_cfg.sigma_db = cfg.sigma_db;
  model_cfg.wall_aware = cfg.wall_aware;
  const RssiModel model(venue, model_cfg);
  const double info_per_packet = 1.0 / (cfg.sigma_db * cfg.sigma_db);

  std::vector<PointScore> out;
  out.reserve(points.size());
  infos.clear();
  infos.reserve(points.size());
  for (const Location& at : points) {
    Info j;
    int heard = 0;
    double best_same = -kInf, best_other = -kInf;
    for (const Beacon& b : venue.beacons()) {
      const Vec2 delta = at.pos - b.pos;
      const int slabs = std::abs(b.level - at.level);
      const double dz = slabs ? venue.level(b.level).elevation_m - venue.level(at.level).elevation_m : 0.0;
      const double range = std::hypot(delta.norm(), dz);
      // Walls only lower the RSSI, so the free-space value bounds it and saves the wall query.
      const double upper = model.expected_rssi_free_space(b, range) - venue.floor_attenuation_db() * slabs;
      if (upper < cfg.min_rssi_dbm) continue;
      const double rssi = cfg.wall_aware ? model.expected_rssi(b, at.pos, at.level) : upper;
      if (rssi < cfg.min_rssi_dbm) continue;

      ++heard;
      if (slabs == 0) {
        best_same = std::max(best_same, rssi);
      } else {
        best_other = std::max(best_other, rssi);
      }
      j.oo += info_per_packet;
      if (range < model_cfg.min_distance) continue;  // inside the clamp the model is flat
      const double k = -10.0 * b.path_loss_exp / std::numbers::ln10 / (range * range);
      const Vec2 g = delta * k;  // d(rssi)/d(position)
      j.xx += info_per_packet * g.x * g.x;
      j.xy += info_per_packet * g.x * g.y;
      j.yy += info_per_packet * g.y * g.y;
      j.xo += info_per_packet * g.x;
      j.yo += info_per_packet * g.y;
    }

    PointScore s;
    s.at = at;
    s.heard = heard;
    s.crlb_m = bound_m(j, cfg);
    s.floor_margin_db = std::max(best_same, cfg.min_rssi_dbm) - std::max(best_other, cfg.min_rssi_dbm);
    out.push_back(s);
    infos.push_back(j);
  }
  return out;
}

}  // namespace

std::vector<PointScore> score_points(const Venue& venue, std::span<const Location> points, const AnalysisConfig& cfg) {
  std::vector<Info> infos;
  return score_with_info(venue, points, cfg, infos);
}

std::vector<PointScore> score_path(const Venue& venue, std::span<const Location> path, const AnalysisConfig& cfg) {
  std::vector<Info> infos;
  auto scores = score_with_info(venue, path, cfg, infos);
  if (cfg.window_m <= 0.0) return scores;
  // The window ends at each point and reaches back window_m of arc length.
  std::vector<double> arc(path.size(), 0.0);
  for (std::size_t i = 1; i < path.size(); ++i) arc[i] = arc[i - 1] + distance(path[i - 1].pos, path[i].pos);
  std::size_t first = 0;
  for (std::size_t i = 0; i < scores.size(); ++i) {
    while (arc[i] - arc[first] > cfg.window_m) ++first;
    Info sum;
    for (std::size_t k = first; k <= i; ++k) sum += infos[k];
    // Points are grid_m apart: count one packet per beacon per metre walked, whatever the spacing.
    const double s = cfg.grid_m;
    scores[i].crlb_m = bound_m({sum.xx * s, sum.xy * s, sum.yy * s, sum.xo * s, sum.yo * s, sum.oo * s}, cfg);
  }
  return scores;
}

Summary summarize(std::span<const PointScore> scores, const AnalysisConfig& cfg) {
  Summary s;
  s.points = scores.size();
  if (scores.empty()) return s;
  std::vector<double> crlb, margin;
  crlb.reserve(scores.size());
  margin.reserve(scores.size());
  std::size_t below3 = 0, within = 0, negative = 0;
  double heard_sum = 0.0;
  for (const PointScore& p : scores) {
    heard_sum += p.heard;
    below3 += p.heard < 3;
    within += p.crlb_m <= cfg.target_m;
    negative += p.floor_margin_db < 0.0;
    crlb.push_back(p.crlb_m);
    margin.push_back(p.floor_margin_db);
  }
  const auto n = static_cast<double>(scores.size());
  s.heard_mean = heard_sum / n;
  s.share_heard_below_3 = static_cast<double>(below3) / n;
  s.crlb_p50_m = percentile(crlb, 0.50);
  s.crlb_p95_m = percentile(crlb, 0.95);
  s.share_within_target = static_cast<double>(within) / n;
  s.floor_margin_p05_db = percentile(margin, 0.05);
  s.share_floor_margin_negative = static_cast<double>(negative) / n;
  return s;
}

}  // namespace ipe::plan
