#include <gtest/gtest.h>

#include "ipe/reachable_area.hpp"
#include "test_util.hpp"

using namespace ipe;

TEST(ReachableArea, ARoomWithoutADoorIsClosed) {
  const Venue v = test::corridor_venue();
  const ReachableArea area(v, 0.1, 10.0);
  ASSERT_TRUE(area.any_closed());
  EXPECT_FALSE(area.open({7.5, 12.0}, 0));
  // Only that room: 20 m^2, give or take the cells its walls cut.
  EXPECT_NEAR(area.closed_area(0), 20.0, 1.0);
}

TEST(ReachableArea, EverythingAWalkerCanReachIsOpen) {
  const Venue v = test::corridor_venue();
  const ReachableArea area(v, 0.1, 10.0);
  EXPECT_TRUE(area.open({20.0, 8.0}, 0));   // the corridor, the largest region
  EXPECT_TRUE(area.open({2.5, 12.0}, 0));   // the room with a door
  EXPECT_TRUE(area.open({25.0, 12.0}, 0));  // a hall without a door, but over a tenth of the corridor
  EXPECT_TRUE(area.open({21.0, 5.0}, 0));   // inside the 4 m^2 box: a pocket, left alone
  EXPECT_TRUE(area.open({-5.0, 5.0}, 0));   // off the map is the venue's business
}

TEST(ReachableArea, IsExactNextToAWall) {
  const Venue v = test::corridor_venue();
  const ReachableArea area(v, 0.1, 10.0);
  EXPECT_TRUE(area.open({7.5, 9.95}, 0));  // 5 cm into the corridor
  EXPECT_FALSE(area.open({7.5, 10.05}, 0));
  EXPECT_TRUE(area.open({4.95, 12.0}, 0));  // 5 cm into the room with a door
  EXPECT_FALSE(area.open({5.05, 12.0}, 0));
  EXPECT_FALSE(area.open({9.95, 12.0}, 0));
  EXPECT_TRUE(area.open({10.05, 12.0}, 0));  // 5 cm into the hall
}

TEST(ReachableArea, SizeLimitsDecideWhatCountsAsARoom) {
  const Venue v = test::corridor_venue();
  EXPECT_FALSE(ReachableArea(v, 0.1, 30.0).any_closed());  // the closed room is under the minimum
  EXPECT_FALSE(ReachableArea(v, 0.0, 10.0).any_closed());  // no region is under 0 of the largest
  const ReachableArea wide(v, 0.5, 10.0);                  // the hall is under half the corridor
  EXPECT_FALSE(wide.open({25.0, 12.0}, 0));
  EXPECT_NEAR(wide.closed_area(0), 140.0, 2.0);
}

TEST(ReachableArea, AStairwellOrLiftOpensARoom) {
  // The corridor on two floors, with a lift standing in the room that has no door.
  const std::string level = R"({"elevation_m": %E, "ordinal": %O, "walls": )" + test::corridor_walls() + "}";
  auto lvl = [&](const char* e, const char* o) {
    std::string s = level;
    s.replace(s.find("%E"), 2, e);
    s.replace(s.find("%O"), 2, o);
    return s;
  };
  const Venue v = parse_venue(R"({"name": "corridor2", "bounds": [0, 0, 40, 14], "levels": [)" + lvl("0", "0") + "," +
                              lvl("4", "1") + R"(],
    "connectors": [{"id": "lift", "kind": "elevator", "levels": [0, 1], "area": [[6,11],[9,11],[9,13],[6,13]]}],
    "beacons": [{"id": "W", "x": 3, "y": 3, "level": 0}]})");
  const ReachableArea area(v, 0.1, 10.0);
  EXPECT_FALSE(area.any_closed());
  EXPECT_TRUE(area.open({7.5, 12.0}, 0));
  EXPECT_TRUE(area.open({7.5, 12.0}, 1));
}

TEST(ReachableArea, AVenueWhoseRoomsHaveDoorsHasNoneClosed) {
  const Venue v = test::two_room_venue();
  EXPECT_FALSE(ReachableArea(v, 0.1, 10.0).any_closed());
}
