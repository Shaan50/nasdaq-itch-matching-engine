// itch_stats: message census of an ITCH 5.0 file (raw or .gz).
//
//   itch_stats data/12302019.NASDAQ_ITCH50 [--max N]

#include <algorithm>
#include <cstdio>
#include <vector>

#include "cli.hpp"
#include "ironbook/common/timing.hpp"
#include "ironbook/itch/file_reader.hpp"
#include "ironbook/itch/symbols.hpp"

using namespace ib;

namespace {

struct Census {
  itch::SymbolTable symbols;
  Timestamp first_ts = ~0ull, last_ts = 0;
  std::uint64_t max_ref = 0;
  std::vector<std::uint64_t> adds_by_locate = std::vector<std::uint64_t>(kMaxLocates);

  void stamp(const itch::Header& h) {
    first_ts = std::min(first_ts, h.timestamp);
    last_ts = std::max(last_ts, h.timestamp);
  }
  void on(const itch::StockDirectory& m) { symbols.on(m); stamp(m.h); }
  void on(const itch::SystemEvent& m) {
    stamp(m.h);
    std::printf("  system event '%c' at %s\n", m.event_code, cli::format_time(m.h.timestamp).c_str());
  }
  void on(const itch::AddOrder& m) {
    stamp(m.h);
    max_ref = std::max(max_ref, m.ref);
    ++adds_by_locate[m.h.locate];
  }
  void on(const itch::OrderReplace& m) { max_ref = std::max(max_ref, m.new_ref); }
};

const char* name_of(char t) {
  switch (t) {
    case 'S': return "System Event";
    case 'R': return "Stock Directory";
    case 'H': return "Stock Trading Action";
    case 'Y': return "Reg SHO Restriction";
    case 'L': return "Market Participant Pos";
    case 'V': return "MWCB Decline Level";
    case 'W': return "MWCB Status";
    case 'K': return "IPO Quoting Period";
    case 'J': return "LULD Auction Collar";
    case 'h': return "Operational Halt";
    case 'A': return "Add Order";
    case 'F': return "Add Order (MPID)";
    case 'E': return "Order Executed";
    case 'C': return "Order Executed w/ Price";
    case 'X': return "Order Cancel";
    case 'D': return "Order Delete";
    case 'U': return "Order Replace";
    case 'P': return "Trade (non-cross)";
    case 'Q': return "Cross Trade";
    case 'B': return "Broken Trade";
    case 'I': return "NOII";
    case 'N': return "RPII";
    case 'O': return "Direct Listing Price Disc";
    default: return "?";
  }
}

}  // namespace

int main(int argc, char** argv) {
  cli::Args args(argc, argv);
  if (args.positional().empty()) {
    std::fprintf(stderr, "usage: itch_stats <file.itch|file.gz> [--max N] [--top K]\n");
    return 2;
  }
  const std::string path = args.positional()[0];
  Census c;
  Stopwatch sw;
  const auto st = itch::replay_file(path, c, args.get_u64("max", ~0ull));
  const double sec = sw.elapsed_sec();

  std::printf("\n%-28s %14s %8s\n", "message type", "count", "share");
  std::vector<std::pair<std::uint64_t, char>> rows;
  for (int t = 0; t < 128; ++t)
    if (st.by_type[static_cast<std::size_t>(t)]) rows.emplace_back(st.by_type[static_cast<std::size_t>(t)], static_cast<char>(t));
  std::sort(rows.rbegin(), rows.rend());
  for (auto [n, t] : rows)
    std::printf("  %c %-25s %14llu %7.2f%%\n", t, name_of(t), static_cast<unsigned long long>(n),
                100.0 * static_cast<double>(n) / static_cast<double>(st.messages));

  std::printf("\nmessages        %llu (%s)\n", static_cast<unsigned long long>(st.messages),
              cli::human(static_cast<double>(st.messages)).c_str());
  std::printf("bytes           %llu\n", static_cast<unsigned long long>(st.bytes));
  std::printf("unknown/bad     %llu / %llu\n", static_cast<unsigned long long>(st.unknown),
              static_cast<unsigned long long>(st.bad_length));
  std::printf("symbols         %zu\n", c.symbols.size());
  std::printf("max order ref   %llu\n", static_cast<unsigned long long>(c.max_ref));
  std::printf("time span       %s - %s\n", cli::format_time(c.first_ts).c_str(), cli::format_time(c.last_ts).c_str());
  std::printf("parse time      %.2f s  (%s msg/s, %.2f GB/s)\n", sec,
              cli::human(static_cast<double>(st.messages) / sec).c_str(),
              static_cast<double>(st.bytes) / sec / 1e9);

  const auto top = args.get_u64("top", 15);
  std::vector<std::pair<std::uint64_t, Locate>> busiest;
  for (std::size_t l = 0; l < kMaxLocates; ++l)
    if (c.adds_by_locate[l]) busiest.emplace_back(c.adds_by_locate[l], static_cast<Locate>(l));
  std::sort(busiest.rbegin(), busiest.rend());
  std::printf("\nbusiest symbols by adds:\n");
  for (std::size_t i = 0; i < busiest.size() && i < top; ++i)
    std::printf("  %-8.*s locate=%-5u adds=%llu\n", static_cast<int>(c.symbols.name(busiest[i].second).size()),
                c.symbols.name(busiest[i].second).data(), busiest[i].second,
                static_cast<unsigned long long>(busiest[i].first));
  return 0;
}
