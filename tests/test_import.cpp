#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ipe/eval.hpp"
#include "ipe/geo.hpp"
#include "ipe/trace.hpp"
#include "test_util.hpp"

using namespace ipe;
namespace fs = std::filesystem;

namespace {

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool near(Vec2 a, Vec2 b, double tol) { return distance(a, b) <= tol; }

/// Same levels, walls (as an unordered set, either direction), connectors and
/// beacons, with coordinates within `tol`.
void expect_same_venue(const Venue& a, const Venue& b, double tol = 0.01) {
  EXPECT_EQ(a.name(), b.name());
  EXPECT_NEAR(a.bounds().min_x, b.bounds().min_x, tol);
  EXPECT_NEAR(a.bounds().min_y, b.bounds().min_y, tol);
  EXPECT_NEAR(a.bounds().max_x, b.bounds().max_x, tol);
  EXPECT_NEAR(a.bounds().max_y, b.bounds().max_y, tol);
  EXPECT_DOUBLE_EQ(a.wall_attenuation_db(), b.wall_attenuation_db());
  EXPECT_DOUBLE_EQ(a.floor_attenuation_db(), b.floor_attenuation_db());
  ASSERT_EQ(a.georef().has_value(), b.georef().has_value());
  if (a.georef()) {
    EXPECT_NEAR(a.georef()->origin_lat, b.georef()->origin_lat, 1e-8);
    EXPECT_NEAR(a.georef()->origin_lon, b.georef()->origin_lon, 1e-8);
    EXPECT_NEAR(a.georef()->rotation_deg, b.georef()->rotation_deg, 1e-9);
  }

  ASSERT_EQ(a.level_count(), b.level_count());
  for (int i = 0; i < a.level_count(); ++i) {
    const Level& la = a.level(i);
    const Level& lb = b.level(i);
    EXPECT_EQ(la.ordinal, lb.ordinal);
    EXPECT_EQ(la.name, lb.name);
    EXPECT_DOUBLE_EQ(la.elevation_m, lb.elevation_m);
    EXPECT_DOUBLE_EQ(la.height_m, lb.height_m);
    ASSERT_EQ(la.walls.size(), lb.walls.size()) << "level " << la.ordinal;
    std::vector<bool> used(lb.walls.size(), false);
    for (const Segment& s : la.walls) {
      bool found = false;
      for (std::size_t k = 0; k < lb.walls.size() && !found; ++k) {
        const Segment& t = lb.walls[k];
        if (used[k]) continue;
        if ((near(s.a, t.a, tol) && near(s.b, t.b, tol)) || (near(s.a, t.b, tol) && near(s.b, t.a, tol))) {
          used[k] = found = true;
        }
      }
      EXPECT_TRUE(found) << "wall " << s.a.x << "," << s.a.y << " - " << s.b.x << "," << s.b.y << " on level "
                         << la.ordinal;
    }
  }

  ASSERT_EQ(a.connectors().size(), b.connectors().size());
  for (std::size_t k = 0; k < a.connectors().size(); ++k) {
    const Connector& ca = a.connectors()[k];
    const Connector& cb = b.connectors()[k];
    EXPECT_EQ(ca.id, cb.id);
    EXPECT_EQ(ca.kind, cb.kind);
    EXPECT_EQ(ca.levels, cb.levels);
    ASSERT_EQ(ca.area.size(), cb.area.size());
    for (std::size_t i = 0; i < ca.area.size(); ++i) EXPECT_TRUE(near(ca.area[i], cb.area[i], tol));
  }

  ASSERT_EQ(a.beacons().size(), b.beacons().size());
  for (std::size_t k = 0; k < a.beacons().size(); ++k) {
    const Beacon& ba = a.beacons()[k];
    const Beacon& bb = b.beacons()[k];
    EXPECT_EQ(ba.id, bb.id);
    EXPECT_EQ(ba.level, bb.level);
    EXPECT_TRUE(near(ba.pos, bb.pos, tol)) << ba.id;
    EXPECT_DOUBLE_EQ(ba.tx_power_dbm, bb.tx_power_dbm);
    EXPECT_DOUBLE_EQ(ba.path_loss_exp, bb.path_loss_exp);
  }
}

fs::path fresh_dir(const std::string& name) {
  const fs::path dir = fs::path(testing::TempDir()) / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

const char* const kShippedVenues[] = {"office_floor", "office_two_floor", "airport_hall"};

}  // namespace

TEST(Import, VenueJsonRoundTrip) {
  for (const char* name : kShippedVenues) {
    SCOPED_TRACE(name);
    const Venue v = load_venue_file(test::data_path(std::string("venues/") + name + ".json"));
    expect_same_venue(v, parse_venue(geo::venue_to_json(v)), 1e-9);
  }
}

TEST(Import, GeoJsonRoundTripWithinOneCentimetre) {
  for (const char* name : kShippedVenues) {
    SCOPED_TRACE(name);
    const Venue v = load_venue_file(test::data_path(std::string("venues/") + name + ".json"));
    const auto r = geo::import_geojson(geo::export_geojson(v), geo::Mapping{});
    EXPECT_TRUE(r.warnings.empty());
    expect_same_venue(v, r.venue);
  }
}

TEST(Import, RotatedGeoRefRoundTrips) {
  Venue v = test::two_level_venue();
  v = Venue(v.name(), v.bounds(), v.levels(), v.beacons(), v.connectors(), v.wall_attenuation_db(),
            v.floor_attenuation_db(), GeoRef{51.5, -0.12, 33.0});
  expect_same_venue(v, geo::import_geojson(geo::export_geojson(v), geo::Mapping{}).venue);
}

TEST(Import, ImdfRoundTrip) {
  const Venue v = test::two_level_venue();
  const fs::path dir = fresh_dir("ipe_imdf_roundtrip");
  geo::export_imdf(v, dir.string());
  expect_same_venue(v, geo::import_imdf(dir.string()).venue);
}

TEST(Import, CommittedImdfSampleMatchesVenue) {
  const Venue v = load_venue_file(test::data_path("venues/office_two_floor.json"));
  expect_same_venue(v, geo::import_imdf(test::data_path("imdf/office_two_floor")).venue);
}

TEST(Import, CommittedExportsAreUpToDate) {
  // Regenerate with: ipe-import export data/venues/X.json --out data/geojson/X.geojson
  for (const char* name : kShippedVenues) {
    SCOPED_TRACE(name);
    const Venue v = load_venue_file(test::data_path(std::string("venues/") + name + ".json"));
    EXPECT_EQ(geo::export_geojson(v), slurp(test::data_path(std::string("geojson/") + name + ".geojson")));
  }
  // Regenerate with: ipe-import export-imdf data/venues/office_two_floor.json --out data/imdf/office_two_floor
  const fs::path dir = fresh_dir("ipe_imdf_current");
  geo::export_imdf(load_venue_file(test::data_path("venues/office_two_floor.json")), dir.string());
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string file = entry.path().filename().string();
    EXPECT_EQ(slurp(entry.path().string()), slurp(test::data_path("imdf/office_two_floor/" + file))) << file;
  }
}

TEST(Import, WebDemoRecordingsAreConsistent) {
  // Regenerate with: datasets/msil/export_demo.py
  auto cfg = eval::preset_config("pf");
  eval::apply_config_overrides(cfg, slurp(test::data_path("configs/msil_wifi.json")));
  int traces = 0;
  for (const auto& floor : fs::directory_iterator(test::data_path("real"))) {
    if (!floor.is_directory()) continue;
    SCOPED_TRACE(floor.path().string());
    const Venue v = load_venue_file((floor.path() / "venue.json").string());
    EXPECT_TRUE(v.georef().has_value());
    EXPECT_EQ(geo::export_geojson(v), slurp((floor.path() / "venue.geojson").string()));
    EXPECT_TRUE(fs::exists(floor.path() / "floor.png"));
    for (const auto& file : fs::directory_iterator(floor.path())) {
      if (file.path().extension() != ".csv") continue;
      const Trace t = read_trace_file(file.path().string());
      EXPECT_FALSE(t.truth.empty());
      EXPECT_FALSE(t.imu.empty());
      for (const auto& b : t.ble) ASSERT_TRUE(v.beacon_index(b.beacon_id).has_value()) << b.beacon_id;
      ++traces;
    }
  }
  EXPECT_GE(traces, 1);
}

/// A platform-style export: rooms instead of walls, a door, per-floor stair
/// footprints with unrelated ids, string level numbers, no georef.
TEST(Import, MappingDrivenImportOfRoomsAndDoors) {
  const GeoRef g{48.0, 11.0, 0.0};
  const auto pos = [&](double x, double y) {
    const LatLon ll = g.to_wgs84({x, y});
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%.10f,%.10f]", ll.lon, ll.lat);
    return std::string(buf);
  };
  const auto rect = [&](double x0, double y0, double x1, double y1) {
    return "{\"type\":\"Polygon\",\"coordinates\":[[" + pos(x0, y0) + "," + pos(x1, y0) + "," + pos(x1, y1) + "," +
           pos(x0, y1) + "," + pos(x0, y0) + "]]}";
  };
  const auto feature = [](const std::string& props, const std::string& geom) {
    return "{\"type\":\"Feature\",\"properties\":{" + props + "},\"geometry\":" + geom + "}";
  };
  const std::string fc = "{\"type\":\"FeatureCollection\",\"features\":[" +
                         feature(R"("kind":"outline","floor":"0","name":"G")", rect(0, 0, 20, 10)) + "," +
                         feature(R"("kind":"outline","floor":"1","name":"1")", rect(0, 0, 20, 10)) + "," +
                         feature(R"("kind":"space","floor":0)", rect(0, 0, 10, 10)) + "," +
                         feature(R"("kind":"space","floor":0)", rect(10, 0, 20, 10)) + "," +
                         feature(R"("kind":"doorway","floor":0)", "{\"type\":\"LineString\",\"coordinates\":[" +
                                                                      pos(10, 4.5) + "," + pos(10, 5.5) + "]}") +
                         "," + feature(R"("kind":"staircase","floor":0,"uid":"s-ground")", rect(1, 1, 3, 3)) + "," +
                         feature(R"("kind":"staircase","floor":1,"uid":"s-first")", rect(1.05, 1, 3.05, 3)) + "," +
                         feature(R"("kind":"kiosk","floor":0)", rect(5, 5, 6, 6)) + "]}";

  const auto mapping = geo::parse_mapping(R"({
    "type_property": "kind",
    "types": {"level": ["outline"], "room": ["space"], "opening": ["doorway"], "stairs": ["staircase"]},
    "level_property": "floor",
    "ordinal_property": "floor",
    "id_property": "uid"
  })");
  geo::ImportOptions opts;
  opts.name = "rooms";
  opts.beacon_grid_m = 10.0;
  const auto r = geo::import_geojson(fc, mapping, opts);
  const Venue& v = r.venue;

  EXPECT_NEAR(v.bounds().min_x, 0.0, 0.01);
  EXPECT_NEAR(v.bounds().max_x, 20.0, 0.01);
  ASSERT_EQ(v.level_count(), 2);
  EXPECT_EQ(v.level(1).name, "1");
  EXPECT_DOUBLE_EQ(v.level(1).elevation_m, 4.0);

  // The shared edge at x = 10 is one wall with a door cut out of it.
  EXPECT_TRUE(v.crosses_wall({9.0, 2.0}, {11.0, 2.0}, 0));
  EXPECT_FALSE(v.crosses_wall({9.0, 5.0}, {11.0, 5.0}, 0));
  EXPECT_EQ(v.count_wall_crossings({9.0, 8.0}, {11.0, 8.0}, 0), 1);
  // Level 1 has an outline but no rooms.
  EXPECT_TRUE(v.walls(1).empty());

  ASSERT_EQ(v.connectors().size(), 1u);
  EXPECT_EQ(v.connectors()[0].kind, ConnectorKind::Stairs);
  EXPECT_EQ(v.connectors()[0].levels, (std::vector<int>{0, 1}));
  EXPECT_NE(v.connector_at({2.0, 2.0}, 1), nullptr);

  EXPECT_EQ(v.beacons().size(), 4u);  // 2 x 1 grid cells on each of two levels
  bool warned_kiosk = false;
  for (const auto& w : r.warnings) warned_kiosk |= w.find("kiosk") != std::string::npos;
  EXPECT_TRUE(warned_kiosk);
}

TEST(Import, BeaconSidecarReplacesBeacons) {
  const Venue v = test::two_level_venue();
  geo::ImportOptions opts;
  opts.beacons_json = R"({"beacons": [{"id": "X", "x": 4, "y": 5, "level": 1, "tx_power": -61}]})";
  const Venue w = geo::import_geojson(geo::export_geojson(v), geo::Mapping{}, opts).venue;
  ASSERT_EQ(w.beacons().size(), 1u);
  EXPECT_EQ(w.beacons()[0].level, 1);
  EXPECT_NEAR(w.beacons()[0].pos.x, 4.0, 1e-9);
  EXPECT_DOUBLE_EQ(w.beacons()[0].tx_power_dbm, -61.0);
}

TEST(Import, ExportNeedsGeoRef) { EXPECT_THROW(geo::export_geojson(test::two_room_venue()), std::runtime_error); }

TEST(Import, MappingRejectsUnknownTypes) {
  EXPECT_THROW(geo::parse_mapping(R"({"types": {"walls": ["x"]}})"), std::runtime_error);
  const auto m = geo::parse_mapping(R"({"type_property": "kind", "types": {"wall": "w"}})");
  EXPECT_EQ(m.type_property, "kind");
  EXPECT_EQ(m.wall, std::vector<std::string>{"w"});
  EXPECT_EQ(m.room, std::vector<std::string>{"room"});
}
