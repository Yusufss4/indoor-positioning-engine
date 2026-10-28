#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>

#include "ipe/trace.hpp"

using namespace ipe;

TEST(Trace, RoundTrip) {
  Trace t;
  t.metadata["scenario"] = "unit, test";
  t.imu.push_back({0.02, {0.1, -0.2, 9.9}, {0.001, 0.002, 0.3}, {10.0, 35.0, -30.0}});
  t.ble.push_back({0.05, "B7", -71});
  t.truth.push_back({0.0, {1.5, 2.25}, 0.5});

  std::stringstream ss;
  write_trace(ss, t);
  const Trace r = read_trace(ss);

  EXPECT_EQ(r.metadata.at("scenario"), "unit, test");
  ASSERT_EQ(r.imu.size(), 1u);
  ASSERT_EQ(r.ble.size(), 1u);
  ASSERT_EQ(r.truth.size(), 1u);
  EXPECT_NEAR(r.imu[0].accel.z, 9.9, 1e-4);
  EXPECT_NEAR(r.imu[0].gyro.z, 0.3, 1e-5);
  EXPECT_EQ(r.ble[0].beacon_id, "B7");
  EXPECT_EQ(r.ble[0].rssi, -71);
  EXPECT_NEAR(r.truth[0].pos.y, 2.25, 1e-3);
}

TEST(Trace, StreamsAreSortedOnRead) {
  std::stringstream ss("ble,2.0,A,-70\nble,1.0,B,-80\ngt,0.5,1,1,0\n");
  const Trace r = read_trace(ss);
  ASSERT_EQ(r.ble.size(), 2u);
  EXPECT_EQ(r.ble[0].beacon_id, "B");
  EXPECT_NEAR(r.duration(), 1.5, 1e-12);
}

TEST(Trace, MalformedRowsReportLineNumber) {
  std::stringstream bad_type("# header\nfoo,1,2\n");
  try {
    read_trace(bad_type);
    FAIL() << "expected throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("line 2"), std::string::npos);
  }
  std::stringstream bad_num("ble,abc,A,-70\n");
  EXPECT_THROW(read_trace(bad_num), std::runtime_error);
  std::stringstream short_row("imu,0.1,1,2\n");
  EXPECT_THROW(read_trace(short_row), std::runtime_error);
}
