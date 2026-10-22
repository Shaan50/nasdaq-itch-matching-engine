#include "ironbook/itch/symbols.hpp"

namespace ib::itch {

void SymbolTable::add(Locate loc, std::string_view name) {
  names_[loc] = std::string(name);
  by_name_[std::string(name)] = loc;
}

std::optional<Locate> SymbolTable::find(std::string_view name) const {
  if (auto it = by_name_.find(std::string(name)); it != by_name_.end()) return it->second;
  return std::nullopt;
}

}  // namespace ib::itch
