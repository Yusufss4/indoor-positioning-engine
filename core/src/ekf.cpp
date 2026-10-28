#include "ipe/ekf.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ipe {
namespace {

using Mat3 = Ekf::Mat3;

Mat3 mul(const Mat3& a, const Mat3& b) {
  Mat3 c{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) c[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
  return c;
}

Mat3 transpose(const Mat3& a) {
  Mat3 t{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) t[j * 3 + i] = a[i * 3 + j];
  return t;
}

void symmetrize(Mat3& a) {
  for (int i = 0; i < 3; ++i)
    for (int j = i + 1; j < 3; ++j) a[i * 3 + j] = a[j * 3 + i] = 0.5 * (a[i * 3 + j] + a[j * 3 + i]);
}

}  // namespace

Ekf::Ekf(const Venue& venue, EkfConfig cfg, RssiModelConfig rssi_cfg, BaroConfig baro_cfg)
    : venue_(&venue),
      cfg_(cfg),
      rssi_model_(venue,
                  [&] {
                    // The EKF is map-free by design.
                    rssi_cfg.wall_aware = false;
                    return rssi_cfg;
                  }()),
      altimeter_(baro_cfg) {}

void Ekf::try_initialize(double t) {
  std::erase_if(pending_, [&](const Pending& p) { return t - p.t > cfg_.init_window_s; });

  // Average RSSI per beacon over the window, then a weighted centroid of the
  // strongest beacons gives the initial position.
  const auto n_beacons = venue_->beacons().size();
  std::vector<double> sum(n_beacons, 0.0);
  std::vector<int> count(n_beacons, 0);
  for (const auto& p : pending_) {
    sum[p.beacon] += p.rssi;
    ++count[p.beacon];
  }
  int distinct = 0;
  for (int c : count) distinct += c > 0 ? 1 : 0;
  if (distinct < cfg_.init_min_beacons) return;

  if (venue_->multi_level()) {
    // Level vote: inverse-square weights of the apparent beacon distances.
    std::vector<double> vote(static_cast<std::size_t>(venue_->level_count()), 0.0);
    for (std::size_t i = 0; i < n_beacons; ++i) {
      if (count[i] == 0) continue;
      const auto& b = venue_->beacons()[i];
      const double d = std::max(1.0, rssi_model_.distance_from_rssi(b, sum[i] / count[i]));
      vote[static_cast<std::size_t>(b.level)] += 1.0 / (d * d);
    }
    level_ = static_cast<int>(std::max_element(vote.begin(), vote.end()) - vote.begin());
    if (altimeter_.initialized()) {
      baro_anchor_m_ = venue_->level(level_).elevation_m - altimeter_.altitude();
      baro_anchored_ = true;
    }
  }

  double wx = 0.0, wy = 0.0, wsum = 0.0;
  for (std::size_t i = 0; i < n_beacons; ++i) {
    if (count[i] == 0) continue;
    const auto& b = venue_->beacons()[i];
    if (b.level != level_) continue;
    const double d = std::max(1.0, rssi_model_.distance_from_rssi(b, sum[i] / count[i]));
    const double w = 1.0 / (d * d);
    wx += w * b.pos.x;
    wy += w * b.pos.y;
    wsum += w;
  }
  x_ = {wx / wsum, wy / wsum, heading_known_ ? last_heading_ : 0.0};
  const double ps = cfg_.init_pos_sigma * cfg_.init_pos_sigma;
  const double hs = deg2rad(cfg_.init_heading_sigma_deg);
  P_ = {ps, 0, 0, 0, ps, 0, 0, 0, heading_known_ ? hs * hs : std::numbers::pi * std::numbers::pi};
  initialized_ = true;
  consecutive_rejections_ = 0;
  pending_.clear();
}

bool Ekf::scalar_update(const std::array<double, 3>& H, double innovation, double R, double gate_chi2) {
  std::array<double, 3> PHt{};
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 3; ++k) PHt[i] += P_[i * 3 + k] * H[k];
  const double S = H[0] * PHt[0] + H[1] * PHt[1] + H[2] * PHt[2] + R;
  if (innovation * innovation / S > gate_chi2) return false;

  std::array<double, 3> K{};
  for (int i = 0; i < 3; ++i) K[i] = PHt[i] / S;
  for (int i = 0; i < 3; ++i) x_[i] += K[i] * innovation;
  x_[2] = wrap_angle(x_[2]);

  // Joseph form keeps P symmetric positive semi-definite despite round-off.
  Mat3 IKH{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) IKH[i * 3 + j] = (i == j ? 1.0 : 0.0) - K[i] * H[j];
  P_ = mul(mul(IKH, P_), transpose(IKH));
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) P_[i * 3 + j] += K[i] * R * K[j];
  symmetrize(P_);
  return true;
}

void Ekf::on_step(const StepEvent& step) {
  last_t_ = step.t;
  if (!initialized_) return;

  const double psi = wrap_angle(x_[2] + step.heading_delta);
  const double L = step.length;
  const double c = std::cos(psi);
  const double s = std::sin(psi);
  x_ = {x_[0] + L * c, x_[1] + L * s, psi};

  const Mat3 F = {1, 0, -L * s, 0, 1, L * c, 0, 0, 1};
  // Control noise on (L, dpsi) mapped through G = d(state)/d(L, dpsi).
  const double sl = cfg_.step_length_sigma_rel * L + cfg_.step_length_sigma_abs;
  const double sh = deg2rad(cfg_.heading_sigma_deg);
  const double G[3][2] = {{c, -L * s}, {s, L * c}, {0.0, 1.0}};
  Mat3 Q{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) Q[i * 3 + j] = G[i][0] * G[j][0] * sl * sl + G[i][1] * G[j][1] * sh * sh;

  P_ = mul(mul(F, P_), transpose(F));
  for (int i = 0; i < 9; ++i) P_[i] += Q[i];
  symmetrize(P_);
}

void Ekf::on_heading(double t, double heading, double sigma) {
  last_t_ = t;
  heading_known_ = true;
  last_heading_ = heading;
  if (!initialized_) return;
  scalar_update({0.0, 0.0, 1.0}, wrap_angle(heading - x_[2]), sigma * sigma, cfg_.heading_gate_chi2);
}

void Ekf::on_rssi(double t, std::size_t beacon_index, double rssi) {
  last_t_ = t;
  if (beacon_index >= venue_->beacons().size() || rssi < cfg_.rssi_min_dbm) return;
  if (!initialized_) {
    pending_.push_back({t, beacon_index, rssi});
    try_initialize(t);
    return;
  }

  const Beacon& b = venue_->beacons()[beacon_index];
  const double dx = x_[0] - b.pos.x;
  const double dy = x_[1] - b.pos.y;
  double d2 = std::max(dx * dx + dy * dy, 1.0);
  double slab_loss = 0.0;
  if (b.level != level_) {
    const double dz = venue_->level(b.level).elevation_m - venue_->level(level_).elevation_m;
    d2 += dz * dz;
    slab_loss = venue_->floor_attenuation_db() * std::abs(b.level - level_);
  }
  const double predicted = rssi_model_.expected_rssi_free_space(b, std::sqrt(d2)) - slab_loss;
  const double k = -10.0 * b.path_loss_exp / std::numbers::ln10;
  const std::array<double, 3> H = {k * dx / d2, k * dy / d2, 0.0};
  const double sigma = rssi_model_.config().sigma_db;

  if (scalar_update(H, rssi - predicted, sigma * sigma, cfg_.rssi_gate_chi2)) {
    consecutive_rejections_ = 0;
    return;
  }
  ++rejected_total_;
  if (++consecutive_rejections_ >= cfg_.reinit_after_rejections) {
    // The filter has diverged from what the radios report; start over.
    initialized_ = false;
    pending_.clear();
    pending_.push_back({t, beacon_index, rssi});
  }
}

void Ekf::on_pressure(double t, double hpa) {
  last_t_ = t;
  const double alt = altimeter_.process(t, hpa);
  if (!venue_->multi_level() || !initialized_) return;
  if (!baro_anchored_) {
    baro_anchor_m_ = venue_->level(level_).elevation_m - alt;
    baro_anchored_ = true;
    return;
  }
  level_ = venue_->nearest_level(alt + baro_anchor_m_);
}

Estimate Ekf::estimate() const {
  Estimate e;
  e.t = last_t_;
  e.valid = initialized_;
  e.level = level_;
  e.z = venue_->level_count() > 0 ? venue_->level(level_).elevation_m : 0.0;
  e.pos = {x_[0], x_[1]};
  e.heading = x_[2];
  e.cov_xx = P_[0];
  e.cov_xy = P_[1];
  e.cov_yy = P_[4];
  return e;
}

}  // namespace ipe
