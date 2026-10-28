#pragma once

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace ipe::cli {

/// Minimal "--key value" / "--flag" parser; enough for the tools here.
class Args {
 public:
  Args(int argc, char** argv, std::set<std::string> flags) : flags_(std::move(flags)) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) != 0) throw std::invalid_argument("unexpected argument: " + a);
      a = a.substr(2);
      if (flags_.count(a)) {
        values_[a] = "1";
      } else {
        if (i + 1 >= argc) throw std::invalid_argument("missing value for --" + a);
        values_[a] = argv[++i];
      }
    }
  }

  [[nodiscard]] bool has(const std::string& k) const { return values_.count(k) > 0; }
  [[nodiscard]] std::string get(const std::string& k, const std::string& def = "") const {
    auto it = values_.find(k);
    return it == values_.end() ? def : it->second;
  }
  [[nodiscard]] std::string require(const std::string& k) const {
    if (!has(k)) throw std::invalid_argument("missing required --" + k);
    return values_.at(k);
  }

 private:
  std::set<std::string> flags_;
  std::map<std::string, std::string> values_;
};

template <typename Fn>
int run_main(const char* usage, Fn&& fn) {
  try {
    return fn();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n\n" << usage;
    return 2;
  }
}

}  // namespace ipe::cli
