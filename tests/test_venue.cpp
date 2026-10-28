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
  for (const char* name : {"venues/office_floor.json", "venues/airport_hall.json", "venues/office_two_floor.json"}) {
    const Venue v = load_venue_file(test::data_path(name));
    EXPECT_GT(v.walls().size(), 4u) << name;
    EXPECT_GE(v.beacons().size(), 8u) << name;
    for (const auto& beacon : v.beacons()) EXPECT_TRUE(v.bounds().contains(beacon.pos)) << beacon.id;
    EXPECT_TRUE(v.georef().has_value()) << name;
  }
}

TEST(Venue, LegacySingleFloorFormatIsOneLevel) {
  const Venue v = test::two_room_venue();
  ASSERT_EQ(v.level_count(), 1);
  EXPECT_FALSE(v.multi_level());
  EXPECT_EQ(v.level(0).ordinal, 0);
  EXPECT_DOUBLE_EQ(v.level(0).elevation_m, 0.0);
  EXPECT_EQ(&v.walls(), &v.level(0).walls);
  EXPECT_TRUE(v.connectors().empty());
  EXPECT_FALSE(v.georef().has_value());
  for (const auto& b : v.beacons()) EXPECT_EQ(b.level, 0);
}

TEST(Venue, LevelsAreSortedByOrdinalAndReferencedByIndex) {
  const Venue v = test::two_level_venue();
  ASSERT_EQ(v.level_count(), 2);
  EXPECT_TRUE(v.multi_level());
  EXPECT_EQ(v.level(0).name, "Ground");
  EXPECT_EQ(v.level(1).name, "First");
  EXPECT_EQ(v.level_index(1), 1);
  EXPECT_FALSE(v.level_index(7).has_value());
  EXPECT_EQ(v.beacons()[v.beacon_index("A1").value()].level, 1);
  EXPECT_EQ(v.nearest_level(-1.0), 0);
  EXPECT_EQ(v.nearest_level(2.1), 1);
  EXPECT_EQ(v.nearest_level(9.0), 1);
}

TEST(Venue, WallQueriesArePerLevel) {
  auto venue_json = R"({"bounds": [0, 0, 10, 10], "beacons": [],
    "levels": [{"ordinal": 0, "walls": [[5, 0, 5, 10]]}, {"ordinal": 1, "walls": []}]})";
  const Venue v = parse_venue(venue_json);
  EXPECT_TRUE(v.crosses_wall({2, 5}, {8, 5}, 0));
  EXPECT_FALSE(v.crosses_wall({2, 5}, {8, 5}, 1));
  EXPECT_EQ(v.count_wall_crossings({2, 5}, {8, 5}, 0), 1);
  EXPECT_EQ(v.count_wall_crossings({2, 5}, {8, 5}, 1), 0);
  EXPECT_DOUBLE_EQ(v.level(1).elevation_m, 4.0);  // ordinal * default height
}

TEST(Venue, ConnectorHitTest) {
  const Venue v = test::two_level_venue();
  ASSERT_EQ(v.connectors().size(), 2u);
  const Connector* stairs = v.connector_at({2, 1.5}, 0);
  ASSERT_NE(stairs, nullptr);
  EXPECT_EQ(stairs->id, "stairs");
  EXPECT_EQ(stairs->kind, ConnectorKind::Stairs);
  EXPECT_EQ(v.connector_at({2, 1.5}, 1), stairs);
  const Connector* lift = v.connector_at({19, 9}, 1);
  ASSERT_NE(lift, nullptr);
  EXPECT_EQ(lift->kind, ConnectorKind::Elevator);
  EXPECT_EQ(v.connector_at({5, 5}, 0), nullptr);
  EXPECT_EQ(v.connector_at({4.5, 1.5}, 0), nullptr);
}

TEST(Venue, RejectsInvalidLevelReferences) {
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "levels": [{"ordinal": 0}],
    "beacons": [{"id": "X", "x": 1, "y": 1, "level": 3}]})"),
               std::runtime_error);
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "levels": [{"ordinal": 0}, {"ordinal": 0}], "beacons": []})"),
               std::runtime_error);
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "levels": [{"ordinal": 0}], "walls": [], "beacons": []})"),
               std::runtime_error);
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "levels": [{"ordinal": 0}, {"ordinal": 1}], "beacons": [],
    "connectors": [{"id": "s", "kind": "ladder", "levels": [0, 1], "area": [[0,0],[1,0],[1,1]]}]})"),
               std::invalid_argument);
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "levels": [{"ordinal": 0}, {"ordinal": 1}], "beacons": [],
    "connectors": [{"id": "s", "kind": "stairs", "levels": [0], "area": [[0,0],[1,0],[1,1]]}]})"),
               std::runtime_error);
}

TEST(Geometry, PointInPolygon) {
  const Polygon square = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
  EXPECT_TRUE(point_in_polygon({1, 1}, square));
  EXPECT_FALSE(point_in_polygon({3, 1}, square));
  const Polygon l_shape = {{0, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 4}, {0, 4}};
  EXPECT_TRUE(point_in_polygon({0.5, 3}, l_shape));
  EXPECT_FALSE(point_in_polygon({3, 3}, l_shape));
}
