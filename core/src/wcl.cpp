#include "ipe/wcl.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ipe {

WeightedCentroid::WeightedCentroid(const Venue& venue, WclConfig cfg, RssiModelConfig rssi_cfg)
    : venue_(&venue), cfg_(cfg), rssi_model_(venue, [&] {
        rssi_cfg.wall_aware = false;
        return rssi_cfg;
      }()) {}

void WeightedCentroid::on_rssi(double t, std::size_t beacon_index, double rssi) {
  const auto n_beacons = venue_->beacons().size();
  if (beacon_index >= n_beacons) return;
  window_.push_back({t, beacon_index, rssi});
  while (!window_.empty() && t - window_.front().t > cfg_.window_s) window_.pop_front();

  std::vector<double> sum(n_beacons, 0.0);
  std::vector<int> count(n_beacons, 0);
  for (const auto& s : window_) {
    sum[s.beacon] += s.rssi;
    ++count[s.beacon];
  }

  struct Candidate {
    std::size_t beacon;
    double rssi;
  };
  std::vector<Candidate> heard;
  for (std::size_t i = 0; i < n_beacons; ++i) {
    if (count[i] > 0) heard.push_back({i, sum[i] / count[i]});
  }
  std::sort(heard.begin(), heard.end(), [](const Candidate& a, const Candidate& b) { return a.rssi > b.rssi; });
  const auto weight = [&](const Candidate& c) {
    const auto& b = venue_->beacons()[c.beacon];
    const double d = std::max(0.5, rssi_model_.distance_from_rssi(b, c.rssi));
    return 1.0 / std::pow(d, cfg_.weight_exponent);
  };

  if (venue_->multi_level()) {
    // Floor vote among the strongest beacons, then a centroid of that floor's beacons only.
    std::vector<double> vote(static_cast<std::size_t>(venue_->level_count()), 0.0);
    for (std::size_t k = 0; k < heard.size() && k < static_cast<std::size_t>(cfg_.top_k); ++k) {
      vote[static_cast<std::size_t>(venue_->beacons()[heard[k].beacon].level)] += weight(heard[k]);
    }
    estimate_.level = static_cast<int>(std::max_element(vote.begin(), vote.end()) - vote.begin());
    estimate_.z = venue_->level(estimate_.level).elevation_m;
    std::erase_if(heard, [&](const Candidate& c) { return venue_->beacons()[c.beacon].level != estimate_.level; });
  }
  if (heard.size() > static_cast<std::size_t>(cfg_.top_k)) heard.resize(static_cast<std::size_t>(cfg_.top_k));

  double wx = 0.0, wy = 0.0, wsum = 0.0;
  for (const auto& c : heard) {
    const auto& b = venue_->beacons()[c.beacon];
    const double w = weight(c);
    wx += w * b.pos.x;
    wy += w * b.pos.y;
    wsum += w;
  }
  if (wsum <= 0.0) return;
  estimate_.t = t;
  estimate_.pos = {wx / wsum, wy / wsum};
  estimate_.valid = true;
}

}  // namespace ipe
