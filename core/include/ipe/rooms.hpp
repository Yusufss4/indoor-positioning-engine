#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "ipe/filter.hpp"
#include "ipe/trace.hpp"
#include "ipe/venue.hpp"

namespace ipe {

class ParticleFilter;

struct RoomEventConfig {
  double enter_prob = 0.6;        ///< Room probability that starts the enter dwell.
  double exit_prob = 0.3;         ///< Room probability that starts the exit dwell.
  double enter_dwell_s = 1.5;     ///< Probability must stay >= enter_prob this long before Enter.
  double exit_dwell_s = 1.5;      ///< Probability must stay <= exit_prob this long before Exit.
  double update_period_s = 0.25;  ///< Minimum time between probability updates.
};

enum class RoomEventType { Enter, Exit };

std::string_view to_string(RoomEventType type);

struct RoomEvent {
  double t = 0.0;
  std::size_t room = 0;  ///< Index into Venue::rooms().
  RoomEventType type = RoomEventType::Enter;
  double probability = 0.0;  ///< Room probability at the update that fired the event.
};

/// Turns position estimates into debounced room Enter/Exit events. Every room
/// has its own two-state machine with hysteresis (enter_prob > exit_prob) and
/// a dwell time, so overlapping rooms are tracked independently and a fix
/// jittering across a doorway does not produce a burst of events.
///
/// With a particle filter the room probability is the particle weight inside
/// the room; other filters give 1 or 0 from the mean position. While the
/// estimate is invalid every room has probability 0, so rooms are exited after
/// exit_dwell_s without a fix.
class RoomTracker {
 public:
  RoomTracker(const Venue& venue, RoomEventConfig cfg);

  /// True if an update at `t` would not be skipped by the update_period_s throttle.
  [[nodiscard]] bool due(double t) const {
    return !state_.empty() && (!updated_ || t >= last_update_ + cfg_.update_period_s);
  }
  /// Throttled to update_period_s. `pf` (may be null) is the particle cloud behind `estimate`.
  void update(double t, const Estimate& estimate, const ParticleFilter* pf);
  /// Runs the state machines on given per-room probabilities (one per venue room).
  void update(double t, std::span<const double> probabilities);

  /// Events since the last call, oldest first; exits precede enters at the same time.
  [[nodiscard]] std::vector<RoomEvent> take_events();
  [[nodiscard]] const std::vector<RoomEvent>& pending_events() const { return events_; }

  [[nodiscard]] bool inside(std::size_t room) const { return state_[room].inside; }
  /// Indices of the rooms currently entered, ascending, into `out` (cleared first).
  void current_rooms(std::vector<std::size_t>& out) const;
  /// Room probabilities of the last update.
  [[nodiscard]] std::span<const double> probabilities() const { return prob_; }
  [[nodiscard]] const RoomEventConfig& config() const { return cfg_; }

 private:
  struct State {
    bool inside = false;
    bool pending = false;  ///< The condition for the next transition holds since `since`.
    double since = 0.0;
  };

  void advance(double t);

  const Venue* venue_;
  RoomEventConfig cfg_;
  std::vector<State> state_;
  std::vector<double> prob_;
  std::vector<RoomEvent> events_;
  double last_update_ = 0.0;
  bool updated_ = false;
};

/// Immediate transitions on 1/0 probabilities: no dwell, no throttling. Used for ground truth.
RoomEventConfig truth_room_config();

/// Ground-truth room transitions: Enter when the true position first lies in a
/// room, Exit when it leaves (no dwell, no hysteresis).
std::vector<RoomEvent> truth_room_events(const Venue& venue, std::span<const TruthSample> truth);

}  // namespace ipe
