#include "ipe/live_session.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "ipe/simulator.hpp"
#include "ipe/trace_player.hpp"
#include "test_util.hpp"

namespace ipe {
namespace {

using Reading = LiveSession::Reading;

TEST(LiveSession, ASavedTraceReplaysToWhatTheSessionShowed) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_stairs_up_down.json"));
  const Venue venue = load_venue_file(sc.venue_path);
  const Trace walk = sim::simulate(sc, venue);
  constexpr double kEnd = 40.0;

  // The walk as a phone would deliver it: values with more digits than a trace keeps, times off the
  // millisecond, radio in one-second scans (with a transmitter the venue does not list) or, every third
  // second, as lone packets.
  struct Event {
    double t;
    int kind;  // 0 imu, 1 radio, 2 pressure, 3 fix, 4 mark
    std::size_t first, last;
  };
  std::vector<Event> events;
  for (std::size_t i = 0; i < walk.imu.size() && walk.imu[i].t < kEnd; ++i) events.push_back({walk.imu[i].t, 0, i, i});
  for (std::size_t b = 0; b < walk.ble.size() && walk.ble[b].t < kEnd;) {
    const double second = std::floor(walk.ble[b].t);
    std::size_t e = b;
    while (e < walk.ble.size() && std::floor(walk.ble[e].t) == second) ++e;
    events.push_back({second + 1.0, 1, b, e});
    b = e;
  }
  for (std::size_t p = 0; p < walk.baro.size() && walk.baro[p].t < kEnd; ++p)
    events.push_back({walk.baro[p].t, 2, p, p});
  events.push_back({0.5, 3, 0, 0});
  for (std::size_t g = 0; g < walk.truth.size() && walk.truth[g].t < kEnd; g += 5)
    events.push_back({walk.truth[g].t, 4, g, g});
  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.t < b.t; });

  LiveSession session(venue);
  std::vector<Estimate> shown;
  std::size_t scans = 0, packets = 0;
  for (const Event& ev : events) {
    const double t = ev.t + 0.00037;
    switch (ev.kind) {
      case 0: {
        const ImuSample& s = walk.imu[ev.first];
        session.on_imu(t, {s.accel.x + 1.1e-6, s.accel.y, s.accel.z}, {s.gyro.x, s.gyro.y, s.gyro.z + 1.3e-7}, s.mag);
        break;
      }
      case 1:
        if (static_cast<long>(ev.t) % 3 == 0) {
          for (std::size_t k = ev.first; k < ev.last; ++k, ++packets)
            session.on_ble(t, walk.ble[k].beacon_id, walk.ble[k].rssi + 0.2);
        } else {
          std::vector<Reading> readings;
          for (std::size_t k = ev.first; k < ev.last; ++k)
            readings.push_back({walk.ble[k].beacon_id, walk.ble[k].rssi + 0.2});
          readings.push_back({"w:not-in-the-venue", -71.4});
          session.on_scan(t, readings);
          ++scans;
        }
        break;
      case 2:
        session.on_pressure(t, walk.baro[ev.first].hpa + 2.5e-6);
        break;
      case 3:
        session.on_position_fix(t, walk.truth.front().pos, walk.truth.front().level, 1.0);
        break;
      default: {
        const TruthSample& g = walk.truth[ev.first];
        session.mark(t, g.pos, g.level);
        shown.push_back(session.estimate());
      }
    }
  }
  ASSERT_GT(shown.size(), 30U);
  ASSERT_GT(scans, 20U);
  ASSERT_GT(packets, 100U);

  std::istringstream csv(session.trace_csv());
  const Trace saved = read_trace(csv);
  ASSERT_EQ(saved.truth.size(), shown.size());
  EXPECT_EQ(saved.imu.size(), session.trace().imu.size());
  EXPECT_EQ(saved.ble.size(), session.trace().ble.size());
  EXPECT_EQ(saved.fixes.size(), 1U);
  EXPECT_EQ(std::count_if(saved.ble.begin(), saved.ble.end(),
                          [](const BleSample& s) { return s.beacon_id == "w:not-in-the-venue"; }),
            static_cast<std::ptrdiff_t>(scans));

  PositioningEngine engine(venue, session.engine().config());
  TracePlayer player(venue, saved);
  std::size_t g = 0;
  bool left_the_ground_floor = false;
  while (!player.finished()) {
    if (player.step(engine) != TracePlayer::Event::Truth) continue;
    const Estimate replayed = engine.estimate();
    ASSERT_EQ(replayed.valid, shown[g].valid) << "mark " << g;
    EXPECT_EQ(replayed.pos.x, shown[g].pos.x) << "mark " << g;
    EXPECT_EQ(replayed.pos.y, shown[g].pos.y) << "mark " << g;
    EXPECT_EQ(replayed.level, shown[g].level) << "mark " << g;
    left_the_ground_floor |= replayed.level != 0;
    ++g;
  }
  EXPECT_EQ(g, shown.size());
  EXPECT_TRUE(left_the_ground_floor);  // the walk climbs the stairs within the 40 s
}

TEST(LiveSession, EventsOfOneMillisecondKeepTheirOrder) {
  LiveSession session(test::two_room_venue());
  session.on_pressure(1.0002, 1013.2);
  session.on_ble(1.0003, "A", -60.0);
  session.mark(1.0001, {2, 2}, 0);
  session.on_pressure(0.5, 1013.2);  // a late sample is not allowed to go back in time
  const Trace& tr = session.trace();
  EXPECT_DOUBLE_EQ(tr.baro[0].t, 1.000);
  EXPECT_DOUBLE_EQ(tr.ble[0].t, 1.001);
  EXPECT_DOUBLE_EQ(tr.truth[0].t, 1.002);
  EXPECT_DOUBLE_EQ(tr.baro[1].t, 1.003);
  EXPECT_DOUBLE_EQ(session.time(), 1.003);
}

TEST(LiveSession, RecordsEveryTransmitterAndUsesTheKnownOnes) {
  LiveSession session(test::two_room_venue());
  EXPECT_FALSE(session.estimate().valid);
  session.on_ble(1.0, "someone's headphones", -80.0);
  EXPECT_FALSE(session.estimate().valid);  // nothing the venue lists has been heard

  const std::vector<Reading> scan = {{"A", -58.4}, {"w:unknown", -50.0}, {"B", -71.6}};
  session.on_scan(2.0, scan);
  session.on_scan(3.0, {});  // ignored
  session.on_scan(4.0, scan);
  EXPECT_TRUE(session.estimate().valid);
  EXPECT_LT(session.estimate().pos.x, 10.0);  // in the west room, where A and B are

  const Trace& tr = session.trace();
  ASSERT_EQ(tr.ble.size(), 7U);
  EXPECT_EQ(tr.ble[0].scan, 0U);
  EXPECT_EQ(tr.ble[1].scan, 1U);
  EXPECT_EQ(tr.ble[2].beacon_id, "w:unknown");
  EXPECT_DOUBLE_EQ(tr.ble[1].rssi, -58.0);  // whole dBm, as the file keeps it
  EXPECT_DOUBLE_EQ(tr.ble[3].rssi, -72.0);
  EXPECT_EQ(tr.ble[4].scan, 2U);  // the empty scan took no number
  EXPECT_DOUBLE_EQ(tr.ble[4].t, 4.0);
}

TEST(LiveSession, MarksMeasureTheErrorAndTheVenueChoosesTheSettings) {
  LiveSession session(test::two_room_venue(), R"({"filter": {"cell_m": 1.0}})");
  EXPECT_DOUBLE_EQ(session.engine().config().filter.cell_m, 1.0);
  EXPECT_DOUBLE_EQ(session.mark(0.1, {3, 3}, 0), -1.0);  // no estimate yet
  session.on_position_fix(0.2, {3, 3}, 0, 0.5);
  const double error = session.mark(0.3, {3, 4}, 0);
  EXPECT_GT(error, 0.5);
  EXPECT_LT(error, 1.5);

  session.set_metadata("source", "a test");
  const std::string csv = session.trace_csv();
  EXPECT_NE(csv.find("# source: a test"), std::string::npos);
  EXPECT_NE(csv.find("# venue: two_rooms"), std::string::npos);
  EXPECT_NE(csv.find("fix,0.200,3.000,3.000,0,0.500"), std::string::npos);

  EXPECT_THROW(LiveSession(test::two_room_venue(), R"({"filter": {"no_such_setting": 1}})"), std::invalid_argument);
}

}  // namespace
}  // namespace ipe
