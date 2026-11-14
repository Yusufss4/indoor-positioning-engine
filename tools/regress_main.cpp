#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "cli.hpp"
#include "ipe/regression.hpp"

namespace {
constexpr const char* kUsage = R"(usage: ipe-regress --scenarios DIR (--baseline FILE | --no-baseline) [options]
       ipe-regress --traces MANIFEST.json (--baseline FILE | --no-baseline) [options]

Simulates every scenario, replays it through the engine and compares
CEP50/CEP95 against the stored baseline. Exits 1 if any metric regressed.
With --traces, recorded traces listed in the manifest are replayed instead
(one run each) and results are grouped by the manifest's "group" field.

options:
  --seeds N           runs per scenario (default 3; ignored with --traces)
  --presets LIST      grid, the only filter (kept for existing scripts)
  --update            overwrite the baseline with the current results
  --markdown FILE     write a markdown results table
  --json FILE         write the current results as JSON
  --tolerance REL     allowed relative increase (default 0.10)
  --serial            disable parallel execution
  --config FILE       JSON merge patch file applied to the default config (before --set).
                      Without it, each venue runs with the config recommended for it:
                      the Wi-Fi fingerprint settings when it has training scans
  --set JSON          merge patch applied on top, for A/B experiments
  --no-baseline       only print results, do not compare
)";
}  // namespace

int main(int argc, char** argv) {
  return ipe::cli::run_main(kUsage, [&] {
    using namespace ipe::eval;
    const ipe::cli::Args args(argc, argv, {"help", "update", "serial", "no-baseline"});
    if (args.has("help")) {
      std::cout << kUsage;
      return 0;
    }

    BenchmarkOptions opts;
    opts.seeds = std::stoi(args.get("seeds", "3"));
    opts.parallel = !args.has("serial");
    if (args.has("presets")) check_filter_name(args.get("presets"));
    opts.venue_config = !args.has("config");
    if (args.has("config")) {
      std::ifstream cfg(args.get("config"));
      if (!cfg) throw std::runtime_error("cannot open " + args.get("config"));
      std::stringstream text;
      text << cfg.rdbuf();
      opts.config_patches.push_back(text.str());
    }
    if (args.has("set")) opts.config_patches.push_back(args.get("set"));

    if (args.has("traces")) opts.seeds = 1;
    const auto results = args.has("traces") ? run_trace_benchmark(args.get("traces"), opts)
                                            : run_benchmark(args.require("scenarios"), opts);
    const std::string table = benchmark_to_markdown(results);
    std::cout << table << "\n";
    if (args.has("markdown")) std::ofstream(args.get("markdown")) << table;
    if (args.has("json")) std::ofstream(args.get("json")) << benchmark_to_json(results, opts) << "\n";

    if (args.has("no-baseline")) return 0;
    const auto baseline_path = args.require("baseline");
    if (args.has("update")) {
      std::ofstream(baseline_path) << benchmark_to_json(results, opts) << "\n";
      std::cout << "baseline updated: " << baseline_path << "\n";
      return 0;
    }

    std::ifstream in(baseline_path);
    if (!in) throw std::runtime_error("cannot open baseline " + baseline_path + " (run with --update first)");
    std::stringstream ss;
    ss << in.rdbuf();

    Tolerance tol;
    tol.relative = std::stod(args.get("tolerance", "0.10"));
    int regressions = 0;
    for (const auto& f : compare_to_baseline(results, ss.str(), tol)) {
      if (f.status == Finding::Status::Ok) continue;
      if (f.status == Finding::Status::Regressed) ++regressions;
      std::printf("%-10s %-24s %-9s %-8s baseline=%.3f current=%.3f\n", to_string(f.status).c_str(), f.scenario.c_str(),
                  f.filter.c_str(), f.metric.c_str(), f.baseline, f.current);
    }
    std::cout << (regressions ? "FAIL: " + std::to_string(regressions) + " regression(s)\n"
                              : "PASS: no accuracy regressions\n");
    return regressions ? 1 : 0;
  });
}
