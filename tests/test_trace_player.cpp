#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

#include "ipe/simulator.hpp"
#include "ipe/trace_player.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {

using Event = TracePlayer::Event;

/// Two-room venue (beacons A-D) and a trace with every stream at t = 1.
Trace tied_trace() {
  Trace tr;
  tr.imu.push_back({1.0, {}, {}, {}});
  tr.ble.push_back({1.0, "A", -60.0, 0});
  tr.baro.push_back({1.0, 1000.0});
  tr.fixes.push_back({1.0, {15.0, 3.0}, 0, 1.0});
  tr.truth.push_back({1.0, {5, 5}});
  return tr;
}

std::vector<Event> play_all(TracePlayer& player, PositioningEngine& engine) {
  std::vector<Event> events;
  while (!player.finished()) events.push_back(player.step(engine));
  return events;
}

}  // namespace

TEST(TracePlayer, TiesGoImuRadioPressureFixTruth) {
  const Venue v = test::two_room_venue();
  const Trace tr = tied_trace();
  PositioningEngine engine(v, {});
  TracePlayer player(v, tr);
  EXPECT_DOUBLE_EQ(player.next_time(), 1.0);
  EXPECT_EQ(play_all(player, engine),
            (std::vector<Event>{Event::Imu, Event::Radio, Event::Pressure, Event::Fix, Event::Truth}));
  EXPECT_NEAR(engine.raw_estimate().pos.x, 15.0, 0.5);  // the fix reached the engine, after the packet
  EXPECT_DOUBLE_EQ(player.truth().pos.x, 5.0);
  EXPECT_TRUE(std::isinf(player.next_time()));
  EXPECT_THROW(player.step(engine), std::logic_error);
}

TEST(TracePlayer, DeliversAScanAsOneStepAndSkipsUnknownBeacons) {
  const Venue v = test::two_room_venue();
  Trace tr;
  tr.ble = {{2.0, "A", -60.0, 7},
            {2.0, "nope", -61.0, 7},
            {2.0, "C", -70.0, 7},
            {3.0, "zzz", -65.0, 0},
            {3.5, "B", -62.0, 0}};
  tr.truth.push_back({4.0, {5, 5}});
  PositioningEngine engine(v, {});
  TracePlayer player(v, tr);

  ASSERT_EQ(player.step(engine), Event::Radio);  // the whole scan
  EXPECT_EQ(std::vector<std::size_t>(player.fed_ble().begin(), player.fed_ble().end()),
            (std::vector<std::size_t>{0, 2}));
  ASSERT_EQ(player.step(engine), Event::Radio);  // lone packet from an unknown beacon
  EXPECT_TRUE(player.fed_ble().empty());
  ASSERT_EQ(player.step(engine), Event::Radio);
  EXPECT_EQ(std::vector<std::size_t>(player.fed_ble().begin(), player.fed_ble().end()), (std::vector<std::size_t>{4}));
  EXPECT_EQ(player.step(engine), Event::Truth);
  EXPECT_TRUE(player.finished());
}

TEST(TracePlayer, MatchesFeedingTheEngineByHand) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_stairs_up_down.json"));
  const Venue v = load_venue_file(sc.venue_path);
  Trace tr = sim::simulate(sc, v);
  // The first 30 s are enough: the test is about event order, not the whole walk.
  const auto keep = [](auto& stream) { std::erase_if(stream, [](const auto& s) { return s.t > 30.0; }); };
  keep(tr.imu);
  keep(tr.ble);
  keep(tr.baro);
  keep(tr.truth);

  PositioningEngine played(v, {});
  TracePlayer player(v, tr);
  while (!player.finished()) player.step(played);

  // Everything before the last truth sample, merged by hand in the same order.
  PositioningEngine manual(v, {});
  const double end = tr.truth.back().t;
  std::size_t i = 0, b = 0, p = 0;
  while (true) {
    const double ti = i < tr.imu.size() ? tr.imu[i].t : 1e300;
    const double tb = b < tr.ble.size() ? tr.ble[b].t : 1e300;
    const double tp = p < tr.baro.size() ? tr.baro[p].t : 1e300;
    if (ti <= tb && ti <= tp && ti <= end) {
      manual.on_imu(tr.imu[i++]);
    } else if (tb <= tp && tb <= end) {
      manual.on_ble(tr.ble[b].t, *v.beacon_index(tr.ble[b].beacon_id), tr.ble[b].rssi);
      ++b;
    } else if (tp <= end) {
      manual.on_pressure(tr.baro[p].t, tr.baro[p].hpa);
      ++p;
    } else {
      break;
    }
  }
  const Estimate a = played.estimate(), m = manual.estimate();
  ASSERT_TRUE(a.valid);
  EXPECT_EQ(a.pos.x, m.pos.x);
  EXPECT_EQ(a.pos.y, m.pos.y);
  EXPECT_EQ(a.level, m.level);
  EXPECT_EQ(played.step_count(), manual.step_count());
}
