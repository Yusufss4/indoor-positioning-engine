#include <iostream>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "ipe/simulator.hpp"

namespace {
constexpr const char* kUsage = R"(usage: ipe-sim --scenario FILE --out TRACE.csv [--seed N]

Generates a synthetic trace (IMU, BLE, ground truth) for a scenario file and
prints a JSON summary to stdout.
)";
}

int main(int argc, char** argv) {
  return ipe::cli::run_main(kUsage, [&] {
    const ipe::cli::Args args(argc, argv, {"help"});
    if (args.has("help")) {
      std::cout << kUsage;
      return 0;
    }
    const auto scenario = ipe::sim::load_scenario_file(args.require("scenario"));
    const auto venue = ipe::load_venue_file(scenario.venue_path);
    std::optional<std::uint64_t> seed;
    if (args.has("seed")) seed = std::stoull(args.get("seed"));

    ipe::sim::SimulationReport report;
    const auto trace = ipe::sim::simulate(scenario, venue, seed, &report);
    const auto out = args.require("out");
    ipe::write_trace_file(out, trace);

    const nlohmann::json summary = {{"trace", out},
                                    {"scenario", scenario.name},
                                    {"venue", venue.name()},
                                    {"venue_path", scenario.venue_path},
                                    {"seed", seed.value_or(scenario.seed)},
                                    {"duration_s", trace.duration()},
                                    {"distance_m", report.distance_m},
                                    {"true_steps", report.steps},
                                    {"imu_samples", trace.imu.size()},
                                    {"ble_samples", trace.ble.size()},
                                    {"wall_violations", report.wall_violations}};
    std::cout << summary.dump(2) << "\n";
    return report.wall_violations == 0 ? 0 : 3;
  });
}
