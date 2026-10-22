#pragma once

// Minimal --flag / --key value parser shared by the command-line tools.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ib::cli {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string_view a = argv[i];
      if (a.starts_with("--")) {
        std::string key(a.substr(2));
        if (auto eq = key.find('='); eq != std::string::npos) {
          kv_[key.substr(0, eq)] = key.substr(eq + 1);
        } else if (i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--") {
          kv_[key] = argv[++i];
        } else {
          kv_[key] = "1";
        }
      } else {
        positional_.emplace_back(a);
      }
    }
  }

  bool has(const std::string& k) const { return kv_.contains(k); }
  std::string get(const std::string& k, std::string def = "") const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  std::uint64_t get_u64(const std::string& k, std::uint64_t def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::strtoull(it->second.c_str(), nullptr, 0);
  }
  double get_f64(const std::string& k, double def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::strtod(it->second.c_str(), nullptr);
  }
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::unordered_map<std::string, std::string> kv_;
  std::vector<std::string> positional_;
};

inline std::vector<std::string> split(const std::string& s, char sep = ',') {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// "09:30:00" or "09:30:00.123" -> ns since midnight.
inline std::uint64_t parse_time(const std::string& s) {
  unsigned h = 0, m = 0;
  double sec = 0;
  std::sscanf(s.c_str(), "%u:%u:%lf", &h, &m, &sec);
  return (static_cast<std::uint64_t>(h) * 3600 + m * 60) * 1'000'000'000ull +
         static_cast<std::uint64_t>(sec * 1e9);
}

inline std::string format_time(std::uint64_t ns) {
  char buf[32];
  const std::uint64_t s = ns / 1'000'000'000ull;
  std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%06llu",
                static_cast<unsigned long long>(s / 3600), static_cast<unsigned long long>(s / 60 % 60),
                static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>(ns % 1'000'000'000ull / 1000));
  return buf;
}

inline std::string human(double v) {
  char buf[32];
  if (v >= 1e9) std::snprintf(buf, sizeof buf, "%.2fB", v / 1e9);
  else if (v >= 1e6) std::snprintf(buf, sizeof buf, "%.2fM", v / 1e6);
  else if (v >= 1e3) std::snprintf(buf, sizeof buf, "%.2fK", v / 1e3);
  else std::snprintf(buf, sizeof buf, "%.0f", v);
  return buf;
}

}  // namespace ib::cli
