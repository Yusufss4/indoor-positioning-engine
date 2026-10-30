#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/venue.hpp"

/// Venue interchange with the map formats indoor-mapping platforms use:
/// GeoJSON (driven by a property mapping table) and a subset of Apple's IMDF.
/// See docs/formats.md for the exact subset.
namespace ipe::geo {

/// Serialises a venue to the JSON format read by parse_venue. Coordinates
/// are rounded to 1 mm.
std::string venue_to_json(const Venue& venue);

struct ExportOptions {
  double wall_thickness_m = 0.2;  ///< Walls are written as thin rectangles so 3D viewers can extrude them.
};

/// Writes the venue as a GeoJSON FeatureCollection in WGS84 using the "ipe"
/// property scheme (data/mappings/ipe.json). Throws if the venue has no georef.
std::string export_geojson(const Venue& venue, const ExportOptions& opts = {});

/// Writes the IMDF subset (manifest, venue, level, unit, opening) plus a
/// beacons.json sidecar into `dir`, which must exist. Throws without georef.
void export_imdf(const Venue& venue, const std::string& dir, const ExportOptions& opts = {});

/// Says which GeoJSON properties and values mean what. A map platform's
/// export is supported by writing one of these tables, not code.
struct Mapping {
  std::string type_property = "feature_type";   ///< Property whose value classifies a feature.
  std::vector<std::string> wall{"wall"};        ///< Lines or thin polygons: impassable walls.
  std::vector<std::string> room{"room"};        ///< Polygons whose edges are walls (minus openings).
  std::vector<std::string> opening{"opening"};  ///< Doors: cut out of walls they lie on.
  std::vector<std::string> level{"level"};      ///< Floor outlines carrying ordinal/name/elevation.
  std::vector<std::string> beacon{"beacon"};    ///< Points.
  std::vector<std::string> venue{"venue"};      ///< Carries the name, attenuations and georef.
  std::vector<std::string> stairs{"stairs"};
  std::vector<std::string> elevator{"elevator"};
  std::vector<std::string> escalator{"escalator"};
  std::vector<std::string> space{"space"};  ///< Named polygons for room enter/exit events; not walls.

  std::string level_property = "level";      ///< Ordinal of the floor a feature is on.
  std::string levels_property = "levels";    ///< Ordinals served by a connector feature (optional).
  std::string ordinal_property = "ordinal";  ///< Ordinal of a level feature.
  std::string name_property = "name";
  std::string id_property = "id";
  std::string elevation_property = "elevation_m";
  std::string height_property = "height_m";
  std::string tx_power_property = "tx_power";
  std::string path_loss_property = "path_loss_exp";

  double wall_max_thickness_m = 0.6;  ///< Thinner rectangles become one centre-line wall.
};

/// Parses a mapping table (docs/formats.md). Missing keys keep the defaults,
/// which describe the "ipe" scheme written by export_geojson.
Mapping parse_mapping(std::string_view json_text);
Mapping load_mapping_file(const std::string& path);

struct ImportOptions {
  std::string name;                            ///< Overrides the name found in the data.
  std::optional<GeoRef> georef;                ///< Overrides the georef found in the data.
  std::optional<double> wall_attenuation_db;   ///< Default: from the data, else 3.5.
  std::optional<double> floor_attenuation_db;  ///< Default: from the data, else 15.
  double level_height_m = 4.0;                 ///< Used when levels carry no elevation.
  std::string beacons_json;                    ///< Sidecar beacon list; replaces beacons in the data.
  double beacon_grid_m = 0.0;                  ///< When > 0 and there are no beacons, place them on this grid.
  double connector_merge_m = 1.0;              ///< Connector footprints on different floors this close are one shaft.
  double min_wall_m = 0.01;                    ///< Wall pieces shorter than this (after cutting doors) are dropped.
};

struct ImportResult {
  Venue venue;
  std::vector<std::string> warnings;
};

/// Builds a venue from a GeoJSON FeatureCollection in WGS84. The local frame
/// is anchored at the south-west corner of the map's extent.
ImportResult import_geojson(std::string_view geojson, const Mapping& mapping, const ImportOptions& opts = {});

/// Reads an IMDF directory (venue, level, unit, opening GeoJSON files). Unit
/// categories map to walls (structure, concrete, ...), connectors (stairs,
/// elevator, escalator) or rooms whose outlines are walls; units carrying
/// "ipe:room_id" (written by export_imdf) become venue rooms. A beacons.json
/// sidecar in the directory is used when opts.beacons_json is empty.
ImportResult import_imdf(const std::string& dir, const ImportOptions& opts = {});

}  // namespace ipe::geo
