#include "ipe/rooms.hpp"

#include <algorithm>
#include <stdexcept>

#include "ipe/particle_filter.hpp"

namespace ipe {

std::string_view to_string(RoomEventType type) { return type == RoomEventType::Enter ? "enter" : "exit"; }

RoomEventConfig truth_room_config() {
  RoomEventConfig cfg;
  cfg.enter_prob = 0.5;
  cfg.exit_prob = 0.5;
  cfg.enter_dwell_s = 0.0;
  cfg.exit_dwell_s = 0.0;
  cfg.update_period_s = 0.0;
  return cfg;
}

RoomTracker::RoomTracker(const Venue& venue, RoomEventConfig cfg)
    : venue_(&venue), cfg_(cfg), state_(venue.rooms().size()), prob_(venue.rooms().size(), 0.0) {
  events_.reserve(16);
}

void RoomTracker::update(double t, const Estimate& estimate, const ParticleFilter* pf) {
  if (!due(t)) return;
  std::fill(prob_.begin(), prob_.end(), 0.0);
  const auto& rooms = venue_->rooms();
  if (estimate.valid && pf) {
    double total = 0.0;
    for (const auto& p : pf->particles()) {
      total += p.weight;
      for (std::size_t r : venue_->room_candidates(p.pos, p.level)) {
        if (rooms[r].contains(p.pos)) prob_[r] += p.weight;
      }
    }
    if (total > 0.0) {
      for (double& v : prob_) v /= total;
    }
  } else if (estimate.valid) {
    for (std::size_t r : venue_->room_candidates(estimate.pos, estimate.level)) {
      if (rooms[r].contains(estimate.pos)) prob_[r] = 1.0;
    }
  }
  advance(t);
}

void RoomTracker::update(double t, std::span<const double> probabilities) {
  if (probabilities.size() != state_.size()) throw std::invalid_argument("RoomTracker: one probability per room");
  if (!due(t)) return;
  std::copy(probabilities.begin(), probabilities.end(), prob_.begin());
  advance(t);
}

void RoomTracker::advance(double t) {
  updated_ = true;
  last_update_ = t;

  const std::size_t first_new = events_.size();
  for (std::size_t r = 0; r < state_.size(); ++r) {
    State& s = state_[r];
    const double p = prob_[r];
    const bool want = s.inside ? p <= cfg_.exit_prob : p >= cfg_.enter_prob;
    if (!want) {
      s.pending = false;
      continue;
    }
    if (!s.pending) {
      s.pending = true;
      s.since = t;
    }
    if (t - s.since < (s.inside ? cfg_.exit_dwell_s : cfg_.enter_dwell_s)) continue;
    events_.push_back({t, r, s.inside ? RoomEventType::Exit : RoomEventType::Enter, p});
    s.inside = !s.inside;
    s.pending = false;
  }
  if (events_.size() - first_new > 1) {
    std::stable_partition(events_.begin() + static_cast<std::ptrdiff_t>(first_new), events_.end(),
                          [](const RoomEvent& e) { return e.type == RoomEventType::Exit; });
  }
}

std::vector<RoomEvent> RoomTracker::take_events() {
  if (events_.empty()) return {};
  std::vector<RoomEvent> out;
  out.swap(events_);
  return out;
}

void RoomTracker::current_rooms(std::vector<std::size_t>& out) const {
  out.clear();
  for (std::size_t r = 0; r < state_.size(); ++r) {
    if (state_[r].inside) out.push_back(r);
  }
}

std::vector<RoomEvent> truth_room_events(const Venue& venue, std::span<const TruthSample> truth) {
  RoomTracker tracker(venue, truth_room_config());
  for (const auto& s : truth) {
    Estimate e;
    e.t = s.t;
    e.pos = s.pos;
    e.level = s.level;
    e.valid = true;
    tracker.update(s.t, e, nullptr);
  }
  return tracker.take_events();
}

}  // namespace ipe
