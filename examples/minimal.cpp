// The smallest program that uses the engine: load a venue, feed sensor data, read the position.
//
//   ./build/examples/ipe-minimal VENUE.json TRACE.csv [CONFIG_PATCH.json]
//
// for example with data/demo/msil/site1_F2/venue.json, the .csv next to it and data/configs/fingerprint.json.
//
// It links ipe::core only. In an app the four on_*() calls below come from the phone's sensor callbacks;
// here a recorded trace supplies them, and its surveyed truth gives an error to print.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ipe/config.hpp"
#include "ipe/engine.hpp"
#include "ipe/trace.hpp"
#include "ipe/trace_player.hpp"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: ipe-minimal VENUE.json TRACE.csv [CONFIG_PATCH.json]\n";
    return 2;
  }
  // The map: walls, floors, beacons or Wi-Fi access points, optionally training scans (docs/formats.md).
  const ipe::Venue venue = ipe::load_venue_file(argv[1]);

  // The default config is the engine as shipped; a JSON merge patch changes what a deployment needs.
  ipe::EngineConfig config;
  if (argc > 3) {
    std::ifstream file(argv[3]);
    std::stringstream patch;
    patch << file.rdbuf();
    ipe::apply_config_patch(config, patch.str());
  }

  ipe::PositioningEngine engine(venue, config);

  // TracePlayer calls, in time order:
  //   engine.on_imu(sample)                accelerometer, gyroscope, magnetometer at about 50 Hz
  //   engine.on_scan(t, readings)          one Wi-Fi scan: {beacon index, dBm} per access point
  //   engine.on_ble(t, beacon_id, rssi)    one BLE advertisement
  //   engine.on_pressure(t, hpa)           barometer, on venues with several floors
  const ipe::Trace trace = ipe::read_trace_file(argv[2]);
  ipe::TracePlayer player(venue, trace);
  while (!player.finished()) {
    if (player.step(engine) != ipe::TracePlayer::Event::Truth) continue;
    const ipe::TruthSample& truth = player.truth();
    const ipe::Estimate fix = engine.estimate();  // what to draw: position, level, covariance
    if (!fix.valid) {
      std::printf("t=%6.1f s  no fix yet\n", truth.t);
      continue;
    }
    std::printf("t=%6.1f s  x=%7.2f m  y=%7.2f m  floor %d  +-%.1f m  error %.1f m\n", truth.t, fix.pos.x, fix.pos.y,
                venue.level(fix.level).ordinal, fix.sigma_radius(), ipe::distance(fix.pos, truth.pos));
  }
  return 0;
}
