#include <gtest/gtest.h>

#include <stdexcept>

#include "ipe/geometry.hpp"
#include "ipe/rng.hpp"
#include "ipe/venue.hpp"
#include "test_util.hpp"

using namespace ipe;

TEST(Venue, ParsesBoundsWallsAndBeacons) {
  const Venue v = test::two_room_venue();
  EXPECT_EQ(v.name(), "two_rooms");
  EXPECT_EQ(v.walls().size(), 6u);
  EXPECT_EQ(v.beacons().size(), 4u);
  EXPECT_DOUBLE_EQ(v.wall_attenuation_db(), 4.0);
  EXPECT_DOUBLE_EQ(v.beacons()[0].tx_power_dbm, -59.0);
  EXPECT_EQ(v.beacon_index("C"), 2u);
  EXPECT_FALSE(v.beacon_index("nope").has_value());
}

TEST(Venue, WallBlocksButDoorDoesNot) {
  const Venue v = test::two_room_venue();
  EXPECT_TRUE(v.crosses_wall({9, 2}, {11, 2}));
  EXPECT_FALSE(v.crosses_wall({9, 5}, {11, 5}));
  EXPECT_FALSE(v.crosses_wall({2, 2}, {8, 8}));
}

TEST(Venue, CountsWallCrossings) {
  const Venue v = test::two_room_venue();
  EXPECT_EQ(v.count_wall_crossings({2, 2}, {18, 2}), 1);
  EXPECT_EQ(v.count_wall_crossings({2, 5}, {18, 5}), 0);
}

// The uniform grid must give exactly the brute-force answer for any query,
// including long radio paths and walls spanning many cells.
TEST(Venue, GridMatchesBruteForce) {
  const Venue v = load_venue_file(test::data_path("venues/office_floor.json"));
  Rng rng(3);
  const auto& b = v.bounds();
  for (int i = 0; i < 5000; ++i) {
    const Vec2 p{rng.uniform(b.min_x, b.max_x), rng.uniform(b.min_y, b.max_y)};
    const double len = i % 2 ? rng.uniform(0.0, 1.5) : rng.uniform(0.0, 50.0);
    const double ang = rng.uniform(-3.14159, 3.14159);
    const Vec2 q = p + Vec2{std::cos(ang), std::sin(ang)} * len;
    int brute = 0;
    for (const auto& w : v.walls()) brute += segments_intersect(p, q, w.a, w.b) ? 1 : 0;
    ASSERT_EQ(v.count_wall_crossings(p, q), brute) << "query " << i;
    ASSERT_EQ(v.crosses_wall(p, q), brute > 0) << "query " << i;
  }
}

TEST(Venue, RejectsMalformedInput) {
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10], "beacons": []})"), std::runtime_error);
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 0, 10], "beacons": []})"), std::runtime_error);
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "walls": [[1, 2, 3]], "beacons": []})"), std::runtime_error);
  EXPECT_ANY_THROW(parse_venue("not json"));
}

TEST(Venue, ShippedVenuesLoad) {
  for (const char* name : {"venues/office_floor.json", "venues/airport_hall.json"}) {
    const Venue v = load_venue_file(test::data_path(name));
    EXPECT_GT(v.walls().size(), 4u) << name;
    EXPECT_GE(v.beacons().size(), 8u) << name;
    for (const auto& beacon : v.beacons()) EXPECT_TRUE(v.bounds().contains(beacon.pos)) << beacon.id;
  }
}
