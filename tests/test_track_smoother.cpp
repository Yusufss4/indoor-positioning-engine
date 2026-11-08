#include <gtest/gtest.h>

#include <cmath>

#include "ipe/engine.hpp"
#include "ipe/eval.hpp"
#include "ipe/simulator.hpp"
#include "ipe/trace_player.hpp"
#include "ipe/track_smoother.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {

Estimate at(Vec2 pos, int level = 0, double t = 0.0) {
  Estimate e;
  e.t = t;
  e.pos = pos;
  e.level = level;
  e.valid = true;
  e.cov_xx = e.cov_yy = 1.0;
  return e;
}

TrackSmootherConfig plain() {
  TrackSmootherConfig cfg;
  cfg.tau_s = 1.0;
  cfg.max_correction_mps = 0.0;
  cfg.snap_m = 6.0;
  cfg.extrapolate_steps = 0.0;
  return cfg;
}

}  // namespace

TEST(TrackSmoother, StepsMoveTheDotByTheFilterDisplacement) {
  const Venue v = test::two_room_venue();
  TrackSmoother s(v, plain());
  s.on_update(0.0, at({2.0, 5.0}));
  for (int k = 1; k <= 5; ++k) {
    const Estimate before = at({2.0 + 0.7 * (k - 1), 5.0});
    const Estimate after = at({2.0 + 0.7 * k, 5.0});
    s.on_step(0.5 * k, before, after);
    const Estimate e = s.estimate(0.5 * k, after);
    EXPECT_NEAR(e.pos.x, after.pos.x, 1e-9);
    EXPECT_NEAR(e.pos.y, 5.0, 1e-9);
  }
}

TEST(TrackSmoother, SmallCorrectionsAreAbsorbedGradually) {
  const Venue v = test::two_room_venue();
  TrackSmoother s(v, plain());
  s.on_update(0.0, at({3.0, 5.0}));
  const Estimate pf = at({3.0, 6.0});  // 1 m north, in the same room
  s.on_update(0.1, pf);
  const Estimate e = s.estimate(0.1, pf);
  EXPECT_NEAR(e.pos.y, 5.0 + (1.0 - std::exp(-0.1)), 1e-9);
  // The covariance is taken about the drawn point.
  EXPECT_NEAR(e.cov_yy, 1.0 + (e.pos.y - 6.0) * (e.pos.y - 6.0), 1e-9);
  for (int k = 2; k <= 50; ++k) s.on_update(0.1 * k, pf);
  EXPECT_NEAR(s.estimate(5.0, pf).pos.y, 6.0, 0.01);
}

TEST(TrackSmoother, CorrectionSpeedIsCapped) {
  const Venue v = test::two_room_venue();
  TrackSmootherConfig cfg = plain();
  cfg.tau_s = 0.01;
  cfg.max_correction_mps = 1.0;
  TrackSmoother s(v, cfg);
  s.on_update(0.0, at({3.0, 3.0}));
  const Estimate pf = at({3.0, 7.0});
  s.on_update(0.5, pf);
  EXPECT_NEAR(s.estimate(0.5, pf).pos.y, 3.5, 1e-9);
}

TEST(TrackSmoother, FarCorrectionsSnap) {
  const Venue v = test::two_room_venue();
  TrackSmoother s(v, plain());
  s.on_update(0.0, at({2.0, 2.0}));
  const Estimate pf = at({8.0, 9.0});  // 9.2 m away
  s.on_update(0.1, pf);
  EXPECT_NEAR(s.estimate(0.1, pf).pos.x, 8.0, 1e-12);
  EXPECT_NEAR(s.estimate(0.1, pf).pos.y, 9.0, 1e-12);
}

TEST(TrackSmoother, NeverGlidesThroughAWall) {
  // West room to east room, away from the door: the dot jumps instead of crossing x = 10.
  const Venue v = test::two_room_venue();
  TrackSmoother s(v, plain());
  s.on_update(0.0, at({9.0, 2.0}));
  const Estimate pf = at({11.0, 2.0});
  for (int k = 1; k <= 20; ++k) {
    s.on_update(0.05 * k, pf);
    const double x = s.estimate(0.05 * k, pf).pos.x;
    EXPECT_TRUE(x < 10.0 || x == 11.0) << x;
  }
  EXPECT_EQ(s.estimate(1.0, pf).pos.x, 11.0);
}

TEST(TrackSmoother, ALevelChangeSnaps) {
  const Venue v = test::two_level_venue();
  TrackSmoother s(v, plain());
  s.on_update(0.0, at({2.0, 1.5}, 0));
  const Estimate pf = at({3.0, 1.5}, 1);
  s.on_update(0.1, pf);
  const Estimate e = s.estimate(0.1, pf);
  EXPECT_EQ(e.level, 1);
  EXPECT_EQ(e.pos.x, 3.0);
}

TEST(TrackSmoother, ExtrapolatesAtTheLastStepsPace) {
  const Venue v = test::two_room_venue();
  TrackSmootherConfig cfg = plain();
  cfg.extrapolate_steps = 0.5;
  TrackSmoother s(v, cfg);
  s.on_update(0.0, at({2.0, 5.0}));
  s.on_step(1.0, at({2.0, 5.0}), at({2.7, 5.0}));  // first step: no pace yet
  s.on_step(1.5, at({2.7, 5.0}), at({3.4, 5.0}));  // 0.7 m in 0.5 s
  const Estimate pf = at({3.4, 5.0});
  EXPECT_NEAR(s.estimate(1.5, pf).pos.x, 3.4, 1e-9);
  EXPECT_NEAR(s.estimate(1.6, pf).pos.x, 3.4 + 0.14, 1e-9);
  EXPECT_NEAR(s.estimate(1.9, pf).pos.x, 3.4 + 0.35, 1e-9);  // capped at half a step
  // A walker who stopped: the pace of a step 2 s after the previous one is not used.
  s.on_step(3.5, at({3.4, 5.0}), at({4.1, 5.0}));
  const Estimate pf2 = at({4.1, 5.0});
  EXPECT_NEAR(s.estimate(3.7, pf2).pos.x, 4.1, 1e-9);
}

TEST(TrackSmoother, InvalidFilterEstimatePassesThrough) {
  const Venue v = test::two_room_venue();
  TrackSmoother s(v, plain());
  Estimate pf = at({2.0, 2.0});
  pf.valid = false;
  s.on_update(0.0, pf);
  EXPECT_FALSE(s.estimate(0.0, pf).valid);
}

TEST(TrackSmoother, DisabledEngineReportsTheFilterEstimate) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_nominal.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto trace = sim::simulate(sc, venue);
  for (const bool enabled : {false, true}) {
    EngineConfig cfg = eval::preset_config("pf");
    cfg.smoothing.enabled = enabled;
    PositioningEngine engine(venue, cfg);
    TracePlayer player(venue, trace);
    double moved = 0.0;
    while (!player.finished()) {
      player.step(engine);
      const Estimate a = engine.estimate();
      const Estimate b = engine.raw_estimate();
      ASSERT_EQ(a.valid, b.valid);
      ASSERT_EQ(a.level, b.level);
      if (!enabled) {
        ASSERT_EQ(a.pos.x, b.pos.x);
        ASSERT_EQ(a.pos.y, b.pos.y);
        ASSERT_EQ(a.cov_xx, b.cov_xx);
      } else if (a.valid) {
        moved = std::max(moved, distance(a.pos, b.pos));
        ASSERT_LE(distance(a.pos, b.pos), cfg.smoothing.snap_m + 1.0);
      }
    }
    if (enabled) {
      EXPECT_GT(moved, 0.0);
    }
  }
}
