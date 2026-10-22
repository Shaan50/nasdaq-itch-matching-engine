#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ironbook/common/types.hpp"
#include "ironbook/itch/messages.hpp"

namespace ib::itch {

// Locate code <-> ticker mapping, built from Stock Directory ('R') messages that NASDAQ
// sends for every instrument before the open.
class SymbolTable {
 public:
  SymbolTable() : names_(kMaxLocates) {}

  void on(const StockDirectory& m) { add(m.h.locate, trim(m.stock)); }
  void add(Locate loc, std::string_view name);

  std::string_view name(Locate loc) const noexcept { return names_[loc]; }
  std::optional<Locate> find(std::string_view name) const;
  std::size_t size() const noexcept { return by_name_.size(); }

 private:
  std::vector<std::string> names_;
  std::unordered_map<std::string, Locate> by_name_;
};

}  // namespace ib::itch
