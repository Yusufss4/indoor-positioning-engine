#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

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
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "footprint": [[0, 0], [5, 5]], "beacons": []})"),
               std::runtime_error);
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "footprint": [[0, 0, 1], [5, 0], [5, 5]], "beacons": []})"),
               std::runtime_error);
}

TEST(Venue, FootprintLimitsTheWalkableArea) {
  const Venue v = parse_venue(R"({"bounds": [0, 0, 20, 10], "beacons": [],
                                  "footprint": [[0, 0], [20, 0], [20, 5], [10, 5], [10, 10], [0, 10]]})");
  ASSERT_EQ(v.footprint().size(), 6u);
  EXPECT_TRUE(v.inside({15, 2}));
  EXPECT_TRUE(v.inside({5, 8}));
  EXPECT_FALSE(v.inside({15, 8}));
  EXPECT_FALSE(v.inside({-1, 2}));
  EXPECT_DOUBLE_EQ(v.walkable_area(), 150.0);

  const Venue plain = test::two_room_venue();
  EXPECT_TRUE(plain.footprint().empty());
  EXPECT_TRUE(plain.inside({15, 8}));
  EXPECT_DOUBLE_EQ(plain.walkable_area(), 200.0);
}

TEST(Venue, LevelFootprintLimitsOneLevel) {
  // A 20 x 10 m ground floor and a 4 x 10 m landing at its east end.
  const Venue v = parse_venue(R"({"bounds": [0, 0, 20, 10], "beacons": [],
                                  "levels": [{"ordinal": 0},
                                             {"ordinal": 1, "footprint": [[16, 0], [20, 0], [20, 10], [16, 10], [16, 0]]}]})");
  EXPECT_TRUE(v.has_level_footprints());
  ASSERT_EQ(v.level(1).footprint.size(), 4u);  // closing point dropped
  EXPECT_TRUE(v.inside({5, 5}, 0));
  EXPECT_FALSE(v.inside({5, 5}, 1));
  EXPECT_TRUE(v.inside({18, 5}, 1));
  EXPECT_TRUE(v.inside({5, 5}));  // without a level: the venue footprint only
  EXPECT_DOUBLE_EQ(v.walkable_area(0), 200.0);
  EXPECT_DOUBLE_EQ(v.walkable_area(1), 40.0);
  const Bounds box = v.sample_box(1);
  EXPECT_DOUBLE_EQ(box.min_x, 16.0);
  EXPECT_DOUBLE_EQ(box.max_x, 20.0);
  EXPECT_FALSE(test::two_level_venue().has_level_footprints());

  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 10, 10], "beacons": [],
                               "levels": [{"ordinal": 0, "footprint": [[0, 0], [5, 5]]}]})"),
               std::runtime_error);
}

TEST(Venue, FootprintCoveringTheBoundsIsDropped) {
  const Venue v = test::with_footprint(test::two_room_venue(), {{0, 0}, {20, 0}, {20, 9.95}, {0, 10}});
  EXPECT_TRUE(v.footprint().empty());
  EXPECT_DOUBLE_EQ(v.walkable_area(), 200.0);
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

namespace {

/// Rooms on `level` containing `p`, ascending, through the venue's room grid.
std::vector<std::size_t> rooms_at(const Venue& v, Vec2 p, int level) {
  std::vector<std::size_t> out;
  for (std::size_t r : v.room_candidates(p, level)) {
    if (v.rooms()[r].contains(p)) out.push_back(r);
  }
  return out;
}

}  // namespace

TEST(Venue, RoomsParseAndHitTest) {
  const Venue v = parse_venue(R"({"bounds": [0, 0, 20, 10], "beacons": [],
    "levels": [{"ordinal": 0}, {"ordinal": 1}],
    "rooms": [
      {"id": "w", "name": "West", "level": 0, "area": [[0,0],[10,0],[10,10],[0,10],[0,0]]},
      {"id": "e", "area": [[10,0],[20,0],[20,10],[10,10]]},
      {"id": "nook", "name": "Nook", "level": 0, "area": [[1,1],[3,1],[3,3],[1,3]]},
      {"id": "up", "name": "Upstairs", "level": 1, "area": [[0,0],[20,0],[20,10],[0,10]]}]})");
  ASSERT_EQ(v.rooms().size(), 4u);
  EXPECT_EQ(v.rooms()[0].name, "West");
  EXPECT_EQ(v.rooms()[0].area.size(), 4u);  // closing point dropped
  EXPECT_EQ(v.rooms()[1].name, "e");        // name defaults to the id
  EXPECT_EQ(v.rooms()[1].level, 0);         // level defaults to the lowest
  EXPECT_EQ(v.rooms()[3].level, 1);
  EXPECT_EQ(rooms_at(v, {2, 2}, 0), (std::vector<std::size_t>{0, 2}));  // rooms may overlap
  EXPECT_EQ(rooms_at(v, {15, 5}, 0), std::vector<std::size_t>{1});
  EXPECT_EQ(rooms_at(v, {15, 5}, 1), std::vector<std::size_t>{3});
  EXPECT_TRUE(rooms_at(v, {25, 5}, 0).empty());
  EXPECT_TRUE(v.room_candidates({25, 5}, 0).empty());
  EXPECT_TRUE(v.room_candidates({5, 5}, 7).empty());
}

TEST(Venue, RoomGridMatchesBruteForce) {
  const Venue v = load_venue_file(test::data_path("venues/kit_informatikbau.json"));
  ASSERT_GT(v.rooms().size(), 100u);
  Rng rng(5);
  for (int i = 0; i < 2000; ++i) {
    const Vec2 p{rng.uniform(v.bounds().min_x - 5, v.bounds().max_x + 5),
                 rng.uniform(v.bounds().min_y - 5, v.bounds().max_y + 5)};
    const int level = std::min(v.level_count() - 1, static_cast<int>(rng.uniform(0.0, v.level_count())));
    const auto fast = rooms_at(v, p, level);
    std::vector<std::size_t> slow;
    for (std::size_t r = 0; r < v.rooms().size(); ++r) {
      if (v.rooms()[r].level == level && point_in_polygon(p, v.rooms()[r].area)) slow.push_back(r);
    }
    EXPECT_EQ(fast, slow) << p.x << "," << p.y;
  }
}

TEST(Venue, RejectsInvalidRooms) {
  const auto with_rooms = [](const std::string& rooms) {
    return parse_venue(R"({"bounds": [0, 0, 10, 10], "levels": [{"ordinal": 0}], "beacons": [], "rooms": )" + rooms +
                       "}");
  };
  EXPECT_NO_THROW(with_rooms(R"([{"id": "a", "area": [[0,0],[1,0],[1,1]]}])"));
  EXPECT_THROW(with_rooms(R"([{"id": "a", "area": [[0,0],[1,0],[1,1]]}, {"id": "a", "area": [[2,2],[3,2],[3,3]]}])"),
               std::runtime_error);
  EXPECT_THROW(with_rooms(R"([{"id": "a", "area": [[0,0],[1,0]]}])"), std::runtime_error);
  EXPECT_THROW(with_rooms(R"([{"id": "a", "area": [[0,0],[1,1],[2,2]]}])"), std::runtime_error);
  EXPECT_THROW(with_rooms(R"([{"id": "a", "level": 2, "area": [[0,0],[1,0],[1,1]]}])"), std::runtime_error);
  EXPECT_THROW(with_rooms(R"([{"id": "", "area": [[0,0],[1,0],[1,1]]}])"), std::runtime_error);
  EXPECT_THROW(with_rooms(R"([{"id": "a", "area": [[0,0,0],[1,0],[1,1]]}])"), std::runtime_error);
}

TEST(Venue, EveryCommittedVenueHasRoomsInsideItsBounds) {
  std::vector<std::string> files;
  for (const char* name : {"office_floor", "office_two_floor", "airport_hall", "kit_informatikbau"}) {
    files.push_back(test::data_path(std::string("venues/") + name + ".json"));
  }
  for (const auto& floor : std::filesystem::directory_iterator(test::data_path("real"))) {
    if (floor.is_directory()) files.push_back((floor.path() / "venue.json").string());
  }
  ASSERT_GT(files.size(), 4u);
  for (const auto& file : files) {
    SCOPED_TRACE(file);
    const Venue v = load_venue_file(file);
    EXPECT_FALSE(v.rooms().empty());
    const Bounds& b = v.bounds();
    for (const Room& r : v.rooms()) {
      EXPECT_FALSE(r.name.empty()) << r.id;
      EXPECT_GE(r.box.min_x, b.min_x - 0.1) << r.id;
      EXPECT_GE(r.box.min_y, b.min_y - 0.1) << r.id;
      EXPECT_LE(r.box.max_x, b.max_x + 0.1) << r.id;
      EXPECT_LE(r.box.max_y, b.max_y + 0.1) << r.id;
    }
  }
}

TEST(Venue, RealDataVenuesLoad) {
  // Venues converted from a public dataset (datasets/ipin).
  const Venue nuremberg = load_venue_file(test::data_path("ipin/nuremberg.json"));
  EXPECT_EQ(nuremberg.level_count(), 3);
  EXPECT_TRUE(nuremberg.has_level_footprints());  // floor -1 is only a stair landing
  EXPECT_FALSE(nuremberg.connectors().empty());
  const Venue uminho = load_venue_file(test::data_path("ipin/uminho.json"));
  EXPECT_EQ(uminho.level_count(), 3);
  EXPECT_FALSE(uminho.connectors().empty());
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

TEST(Venue, ParsesFloorFingerprints) {
  const Venue v = parse_venue(R"({
    "bounds": [0, 0, 10, 10],
    "levels": [{"ordinal": 0, "name": "G", "walls": []}, {"ordinal": 1, "name": "F1", "walls": []}],
    "beacons": [{"id": "a", "x": 1, "y": 1, "level": 0}, {"id": "b", "x": 9, "y": 9, "level": 1}],
    "floor_fingerprints": {"ids": ["b", "gone", "a"], "scans": [[1, 0, -50, 1, -70, 2, -80], [0, 2, -40]]}
  })");
  const auto& fp = v.floor_fingerprints();
  ASSERT_EQ(fp.size(), 2u);
  EXPECT_EQ(fp.level[0], 1);
  EXPECT_EQ(fp.level[1], 0);
  ASSERT_EQ(fp.begin, (std::vector<std::uint32_t>{0, 2, 3}));  // "gone" is not a venue beacon
  EXPECT_EQ(fp.beacon, (std::vector<std::uint32_t>{1, 0, 0}));
  EXPECT_EQ(fp.rssi, (std::vector<float>{-50.0F, -80.0F, -40.0F}));
  EXPECT_TRUE(parse_venue(R"({"bounds": [0, 0, 1, 1], "beacons": []})").floor_fingerprints().empty());
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 1, 1], "beacons": [],
                               "floor_fingerprints": {"ids": [], "scans": [[0, 1]]}})"),
               std::exception);
}

TEST(Venue, ParsesFingerprintPositionsAndUnmodelledBeacons) {
  const Venue v = parse_venue(R"({
    "bounds": [0, 0, 10, 10],
    "beacons": [{"id": "a", "x": 1, "y": 1}, {"id": "b", "x": 9, "y": 9, "model": false}],
    "floor_fingerprints": {"ids": ["a", "b"], "scans": [[0, 0, -50, 1, -70], [0, 1, -40]],
                           "xy": [[2.5, 3.5], [8, 9]]}
  })");
  EXPECT_TRUE(v.beacons()[0].modelled);
  EXPECT_FALSE(v.beacons()[1].modelled);
  const auto& fp = v.floor_fingerprints();
  ASSERT_TRUE(fp.has_positions());
  EXPECT_DOUBLE_EQ(fp.pos[0].x, 2.5);
  EXPECT_DOUBLE_EQ(fp.pos[1].y, 9.0);
  // Positions are optional, but when given there is one per scan.
  EXPECT_FALSE(parse_venue(R"({"bounds": [0, 0, 1, 1], "beacons": [{"id": "a", "x": 0, "y": 0}],
                               "floor_fingerprints": {"ids": ["a"], "scans": [[0, 0, -50]]}})")
                   .floor_fingerprints()
                   .has_positions());
  EXPECT_THROW(parse_venue(R"({"bounds": [0, 0, 1, 1], "beacons": [{"id": "a", "x": 0, "y": 0}],
                               "floor_fingerprints": {"ids": ["a"], "scans": [[0, 0, -50]], "xy": []}})"),
               std::runtime_error);
}
