#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "cli.hpp"
#include "ipe/regression.hpp"

namespace {
constexpr const char* kUsage = R"(usage: ipe-regress --scenarios DIR (--baseline FILE | --no-baseline) [options]

Simulates every scenario, replays it through each filter preset and compares
CEP50/CEP95 against the stored baseline. Exits 1 if any metric regressed.

options:
  --seeds N           runs per scenario (default 3)
  --presets LIST      comma separated presets (default pf,pf-nomap,ekf,wcl)
  --update            overwrite the baseline with the current results
  --markdown FILE     write a markdown results table
  --json FILE         write the current results as JSON
  --tolerance REL     allowed relative increase (default 0.10)
  --serial            disable parallel execution
  --set JSON          merge patch applied to every preset, for A/B experiments
  --no-baseline       only print results, do not compare
)";

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}
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
    if (args.has("presets")) opts.presets = split_csv(args.get("presets"));
    opts.config_patch = args.get("set");

    const auto results = run_benchmark(args.require("scenarios"), opts);
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
                  f.preset.c_str(), f.metric.c_str(), f.baseline, f.current);
    }
    std::cout << (regressions ? "FAIL: " + std::to_string(regressions) + " regression(s)\n"
                              : "PASS: no accuracy regressions\n");
    return regressions ? 1 : 0;
  });
}
