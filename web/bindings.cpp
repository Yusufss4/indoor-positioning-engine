// WebAssembly entry point for the browser demo. The same C++ engine that the
// CLI tools use runs here unchanged; only this thin embind layer is web-specific.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ipe/engine.hpp"
#include "ipe/eval.hpp"
#include "ipe/grid_engine.hpp"
#include "ipe/simulator.hpp"
#include "ipe/trace.hpp"
#include "ipe/trace_player.hpp"

using emscripten::val;

namespace {

/// Live scoring at the truth samples, as replay does it (after a 5 s warm-up).
struct Score {
  double error_sum = 0.0;
  int error_count = 0;
  std::vector<double> errors;
  int floor_ok = 0;  ///< Fixes on the true level (horizontal error is only scored for these).
  int fixes = 0;

  void add(const ipe::Estimate& e, const ipe::TruthSample& truth) {
    if (!e.valid || truth.t < 5.0) return;
    ++fixes;
    if (e.level != truth.level) return;
    ++floor_ok;
    const double err = ipe::distance(e.pos, truth.pos);
    error_sum += err;
    ++error_count;
    errors.push_back(err);
  }
  double floor_accuracy() const { return fixes ? static_cast<double>(floor_ok) / fixes : -1.0; }
};

// Names of the estimates in the JS API.
constexpr const char* kPf = "pf";
constexpr const char* kGrid = "grid";  ///< The grid filter prototype (docs/grid.md), when enabled.

/// Grid cells handed to the page per snapshot, heaviest first, and the mass below which a cell is not drawn
/// (relative to the heaviest cell).
constexpr std::size_t kMaxCells = 2500;
constexpr double kCellFloor = 1e-3;

/// What the trace player feeds: both filters see every event of the one trace.
struct Engines {
  ipe::PositioningEngine* pf = nullptr;
  ipe::GridEngine* grid = nullptr;  ///< Null when the grid filter is off.

  void on_imu(const ipe::ImuSample& s) {
    pf->on_imu(s);
    if (grid) grid->on_imu(s);
  }
  void on_ble(double t, std::size_t beacon, double rssi) {
    pf->on_ble(t, beacon, rssi);
    if (grid) grid->on_ble(t, beacon, rssi);
  }
  void on_scan(double t, std::span<const ipe::RssiReading> readings) {
    pf->on_scan(t, readings);
    if (grid) grid->on_scan(t, readings);
  }
  void on_pressure(double t, double hpa) {
    pf->on_pressure(t, hpa);
    if (grid) grid->on_pressure(t, hpa);
  }
};

class Demo {
 public:
  /// Simulates the scenario and runs the particle filter on it, with `pf_config_patch` (may be
  /// empty) on top of the default config, and the grid filter next to it when `grid` is set.
  Demo(const std::string& venue_json, const std::string& scenario_json, unsigned seed,
       const std::string& pf_config_patch, bool grid)
      : venue_(ipe::parse_venue(venue_json)) {
    trace_ = ipe::sim::simulate(ipe::sim::parse_scenario(scenario_json), venue_, seed);
    init(pf_config_patch, grid);
  }

  /// Replays a recorded trace. `pf_config_patch` (may be empty) is applied on top of the default config.
  static Demo* replay(const std::string& venue_json, const std::string& trace_csv, const std::string& pf_config_patch,
                      bool grid) {
    std::istringstream in(trace_csv);
    return new Demo(ipe::parse_venue(venue_json), ipe::read_trace(in), pf_config_patch, grid);
  }

  double time() const { return now_; }
  double duration() const { return trace_.truth.empty() ? 0.0 : trace_.truth.back().t; }
  bool finished() const { return player_->finished(); }
  int levelCount() const { return venue_.level_count(); }
  bool hasGrid() const { return grid_ != nullptr; }
  /// Side of a grid cell [m]; 0 without the grid filter.
  double gridCellSize() const { return grid_ ? grid_map_->cell_m() : 0.0; }
  int gridCellCount() const { return grid_ ? static_cast<int>(grid_map_->size()) : 0; }

  /// Processes every sensor event up to now + dt. Returns false at the end.
  bool advance(double dt) {
    const double until = now_ + dt;
    recent_ble_.clear();
    recent_rooms_.clear();
    Engines engines{engine_.get(), grid_.get()};
    while (!player_->finished() && player_->next_time() <= until) {
      switch (player_->step(engines)) {
        case ipe::TracePlayer::Event::Radio: {
          const auto fed = player_->fed_ble();
          recent_ble_.insert(recent_ble_.end(), fed.begin(), fed.end());
          break;
        }
        case ipe::TracePlayer::Event::Truth:
          on_truth(player_->truth());
          break;
        default:
          break;
      }
    }
    for (const auto& e : engine_->take_room_events()) recent_rooms_.push_back({kPf, e});
    if (grid_) {
      for (const auto& e : grid_->take_room_events()) recent_rooms_.push_back({kGrid, e});
    }
    std::stable_sort(recent_rooms_.begin(), recent_rooms_.end(),
                     [](const auto& a, const auto& b) { return a.second.t < b.second.t; });
    now_ = until;
    return !finished();
  }

  /// Ground truth at the current time, interpolated between truth samples. Recorded traces only have
  /// sparse surveyor waypoints; scoring (mean, CEP95) still happens at the samples themselves.
  val truth() const {
    const auto t = truth_now();
    val o = val::object();
    o.set("x", t.pos.x);
    o.set("y", t.pos.y);
    o.set("heading", t.heading);
    o.set("level", t.level);
    o.set("z", t.z);
    // Recordings start before the surveyor's first waypoint: until then there is no truth to show.
    o.set("valid", truth_seen_ > 0);
    set_wgs84(o, t.pos);
    return o;
  }

  /// The estimate named `name` ("pf", or "grid" when enabled; anything else gives an empty object) with its
  /// live scores.
  val estimate(const std::string& name) const {
    val o = val::object();
    const auto e = estimate_of(name);
    const Score* score = score_of(name);
    if (!e || !score) return o;
    o.set("valid", e->valid);
    o.set("x", e->pos.x);
    o.set("y", e->pos.y);
    o.set("level", e->level);
    o.set("z", e->z);
    set_wgs84(o, e->pos);
    o.set("sigma", e->sigma_radius());
    const auto t = truth_now();
    o.set("error", e->valid && e->level == t.level ? ipe::distance(e->pos, t.pos) : -1.0);
    o.set("mean", score->error_count ? score->error_sum / score->error_count : -1.0);
    o.set("cep95", ipe::eval::percentile(score->errors, 0.95));
    o.set("floorAcc", score->floor_accuracy());
    return o;
  }

  /// Fraction of fixes (after the 5 s warm-up) on the true level; -1 before the first.
  double floorAccuracy(const std::string& name) const {
    const Score* score = score_of(name);
    return score ? score->floor_accuracy() : -1.0;
  }

  /// Flat [x0, y0, w0, level0, x1, ...] view of the particle cloud. The view
  /// aliases WASM memory and is only valid until the next call.
  val particles() {
    particle_buf_.clear();
    for (const auto& p : engine_->particle_filter().particles()) {
      particle_buf_.push_back(static_cast<float>(p.pos.x));
      particle_buf_.push_back(static_cast<float>(p.pos.y));
      particle_buf_.push_back(static_cast<float>(p.weight));
      particle_buf_.push_back(static_cast<float>(p.level));
    }
    return val(emscripten::typed_memory_view(particle_buf_.size(), particle_buf_.data()));
  }

  /// Flat [x0, y0, m0, level0, x1, ...] view of the grid filter's heaviest cells, heaviest first: cell centre
  /// and mass relative to the heaviest cell. At most kMaxCells, none below kCellFloor of the heaviest. Cells
  /// of equal mass (the uniform prior) are picked spread over the map rather than in index order. Empty
  /// without the grid filter; aliases WASM memory like particles().
  val gridCells() {
    cell_buf_.clear();
    if (grid_) {
      const auto mass = grid_->filter().mass();
      const double top = mass.empty() ? 0.0 : *std::max_element(mass.begin(), mass.end());
      cell_order_.clear();
      for (std::size_t c = 0; c < mass.size(); ++c) {
        if (top > 0.0 && mass[c] >= kCellFloor * top) cell_order_.push_back(static_cast<std::uint32_t>(c));
      }
      // Ties broken by a multiplicative hash of the index, so that equal cells are a spatial sample.
      const auto heavier = [&](std::uint32_t a, std::uint32_t b) {
        if (mass[a] != mass[b]) return mass[a] > mass[b];
        return a * 2654435761u < b * 2654435761u;
      };
      const std::size_t k = std::min(kMaxCells, cell_order_.size());
      std::partial_sort(cell_order_.begin(), cell_order_.begin() + static_cast<std::ptrdiff_t>(k), cell_order_.end(),
                        heavier);
      const ipe::GridMap& map = *grid_map_;
      for (std::size_t i = 0; i < k; ++i) {
        const std::uint32_t c = cell_order_[i];
        const ipe::Vec2 p = map.centre(c);
        cell_buf_.push_back(static_cast<float>(p.x));
        cell_buf_.push_back(static_cast<float>(p.y));
        cell_buf_.push_back(static_cast<float>(mass[c] / top));
        cell_buf_.push_back(static_cast<float>(map.cell(c).level));
      }
    }
    return val(emscripten::typed_memory_view(cell_buf_.size(), cell_buf_.data()));
  }

  /// Beacon packets received during the last advance(): [{id, rssi}].
  val recentBle() const {
    val arr = val::array();
    for (std::size_t k : recent_ble_) {
      val o = val::object();
      o.set("id", trace_.ble[k].beacon_id);
      o.set("rssi", trace_.ble[k].rssi);
      arr.call<void>("push", o);
    }
    return arr;
  }

  /// Room enter/exit events during the last advance(), oldest first:
  /// [{t, filter ("pf", "grid" or "truth"), room (index into the venue's rooms), roomId, name, type}].
  val roomEvents() const {
    val arr = val::array();
    for (const auto& [source, e] : recent_rooms_) {
      const auto& room = venue_.rooms()[e.room];
      val o = val::object();
      o.set("t", e.t);
      o.set("filter", source);
      o.set("room", static_cast<int>(e.room));
      o.set("roomId", room.id);
      o.set("name", room.name);
      o.set("type", std::string(ipe::to_string(e.type)));
      arr.call<void>("push", o);
    }
    return arr;
  }

  /// Indices into the venue's rooms that `name` ("pf", "grid" or "truth") is currently in.
  val currentRooms(const std::string& name) const {
    std::vector<std::size_t> rooms;
    if (name == "truth") {
      if (truth_rooms_) truth_rooms_->current_rooms(rooms);
    } else if (name == kPf) {
      rooms = engine_->current_rooms();
    } else if (name == kGrid && grid_) {
      if (const auto* tracker = grid_->room_tracker()) tracker->current_rooms(rooms);
    }
    val arr = val::array();
    for (std::size_t r : rooms) arr.call<void>("push", static_cast<int>(r));
    return arr;
  }

  int steps() const { return static_cast<int>(engine_->step_count()); }

 private:
  Demo(ipe::Venue venue, ipe::Trace trace, const std::string& pf_config_patch, bool grid)
      : venue_(std::move(venue)), trace_(std::move(trace)) {
    init(pf_config_patch, grid);
  }

  void init(const std::string& pf_config_patch, bool grid) {
    auto cfg = ipe::eval::preset_config("pf");
    if (!pf_config_patch.empty()) ipe::eval::apply_config_overrides(cfg, pf_config_patch);
    engine_ = std::make_unique<ipe::PositioningEngine>(venue_, cfg);
    if (grid) {
      // The grid filter takes PDR, the RSSI model, the barometer, the smoother and the rooms from the same
      // engine config (as ipe-grid --config does) and its own settings at their defaults.
      const ipe::GridFilterConfig gcfg;
      grid_map_ = std::make_shared<const ipe::GridMap>(venue_, gcfg);
      grid_ = std::make_unique<ipe::GridEngine>(venue_, grid_map_, cfg, gcfg);
    }
    // venue_ and trace_ are members and the Demo is never moved, so the player's references stay valid.
    player_.emplace(venue_, trace_);
    if (!venue_.rooms().empty()) truth_rooms_.emplace(venue_, ipe::truth_room_config());
  }

  std::optional<ipe::Estimate> estimate_of(const std::string& name) const {
    if (name == kPf) return engine_->estimate();
    if (name == kGrid && grid_) return grid_->estimate();
    return std::nullopt;
  }

  const Score* score_of(const std::string& name) const {
    if (name == kPf) return &pf_score_;
    if (name == kGrid && grid_) return &grid_score_;
    return nullptr;
  }

  void on_truth(const ipe::TruthSample& truth) {
    truth_ = truth;
    ++truth_seen_;
    if (truth_rooms_) {
      ipe::Estimate at_truth;
      at_truth.pos = truth_.pos;
      at_truth.level = truth_.level;
      at_truth.valid = true;
      truth_rooms_->update(truth_.t, at_truth, nullptr);
      for (const auto& e : truth_rooms_->take_events()) recent_rooms_.push_back({"truth", e});
    }
    pf_score_.add(engine_->estimate(), truth_);
    if (grid_) grid_score_.add(grid_->estimate(), truth_);
  }

  ipe::TruthSample truth_now() const {
    if (truth_seen_ == 0 || truth_seen_ >= trace_.truth.size()) return truth_;
    const auto& next = trace_.truth[truth_seen_];
    if (next.level != truth_.level || next.t <= truth_.t) return truth_;
    const double a = std::clamp((now_ - truth_.t) / (next.t - truth_.t), 0.0, 1.0);
    auto t = truth_;
    t.pos = truth_.pos + (next.pos - truth_.pos) * a;
    return t;
  }

  void set_wgs84(val& o, ipe::Vec2 p) const {
    if (!venue_.georef()) return;
    const auto ll = venue_.georef()->to_wgs84(p);
    o.set("lat", ll.lat);
    o.set("lon", ll.lon);
  }

  ipe::Venue venue_;
  ipe::Trace trace_;
  std::unique_ptr<ipe::PositioningEngine> engine_;
  std::shared_ptr<const ipe::GridMap> grid_map_;
  std::unique_ptr<ipe::GridEngine> grid_;
  std::optional<ipe::TracePlayer> player_;
  Score pf_score_;
  Score grid_score_;
  std::vector<std::size_t> recent_ble_;
  std::vector<std::pair<std::string, ipe::RoomEvent>> recent_rooms_;
  std::optional<ipe::RoomTracker> truth_rooms_;
  std::vector<float> particle_buf_;
  std::vector<float> cell_buf_;
  std::vector<std::uint32_t> cell_order_;
  ipe::TruthSample truth_{};
  double now_ = 0.0;
  std::size_t truth_seen_ = 0;  ///< Truth samples consumed so far.
};

}  // namespace

EMSCRIPTEN_BINDINGS(ipe) {
  emscripten::class_<Demo>("Demo")
      .constructor<std::string, std::string, unsigned, std::string, bool>()
      .class_function("replay", &Demo::replay, emscripten::return_value_policy::take_ownership())
      .function("time", &Demo::time)
      .function("duration", &Demo::duration)
      .function("finished", &Demo::finished)
      .function("levelCount", &Demo::levelCount)
      .function("hasGrid", &Demo::hasGrid)
      .function("gridCellSize", &Demo::gridCellSize)
      .function("gridCellCount", &Demo::gridCellCount)
      .function("advance", &Demo::advance)
      .function("truth", &Demo::truth)
      .function("estimate", &Demo::estimate)
      .function("floorAccuracy", &Demo::floorAccuracy)
      .function("particles", &Demo::particles)
      .function("gridCells", &Demo::gridCells)
      .function("recentBle", &Demo::recentBle)
      .function("roomEvents", &Demo::roomEvents)
      .function("currentRooms", &Demo::currentRooms)
      .function("steps", &Demo::steps);
}
