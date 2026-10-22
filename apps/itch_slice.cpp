// itch_slice: cut a smaller, still self-consistent ITCH file out of a full day.
//
//   itch_slice IN OUT --first N
//       the first N messages (book state is exact because every order's add precedes
//       any reference to it).
//   itch_slice IN OUT --symbols AAPL,MSFT [--max N]
//       every message for the listed symbols over the whole day, plus all non-symbol
//       messages (system events) and the stock directory. Because E/C/X/D/U carry the
//       stock locate, this keeps each order's complete lifecycle.
//   --drop L,Y,I  omit these message types (non-book reference data) from the output.

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "cli.hpp"
#include "ironbook/common/endian.hpp"
#include "ironbook/common/mapped_file.hpp"
#include "ironbook/itch/messages.hpp"
#include "ironbook/itch/parser.hpp"

using namespace ib;

int main(int argc, char** argv) {
  cli::Args args(argc, argv);
  if (args.positional().size() < 2 || (!args.has("first") && !args.has("symbols"))) {
    std::fprintf(stderr, "usage: itch_slice IN OUT (--first N | --symbols A,B,C [--max N]) [--drop T,T]\n");
    return 2;
  }
  MappedFile in(args.positional()[0]);
  FILE* out = std::fopen(args.positional()[1].c_str(), "wb");
  if (!out) {
    std::perror("fopen");
    return 1;
  }

  itch::FrameCursor cur(in.bytes());
  std::uint64_t kept = 0, seen = 0;
  std::vector<bool> drop(128, false);
  for (auto& t : cli::split(args.get("drop"))) drop[static_cast<unsigned char>(t[0]) & 0x7F] = true;
  auto write = [&](std::span<const std::uint8_t> payload) {
    if (drop[payload[0] & 0x7F]) return;
    std::uint8_t len[2];
    be::store16(len, static_cast<std::uint16_t>(payload.size()));
    std::fwrite(len, 1, 2, out);
    std::fwrite(payload.data(), 1, payload.size(), out);
    ++kept;
  };

  if (args.has("first")) {
    const auto n = args.get_u64("first", 0);
    for (auto p = cur.next(); !p.empty() && seen < n; p = cur.next(), ++seen) write(p);
  } else {
    std::unordered_set<std::string> wanted;
    for (auto& s : cli::split(args.get("symbols"))) wanted.insert(s);
    std::vector<bool> keep(kMaxLocates, false);
    const auto max = args.get_u64("max", ~0ull);
    for (auto p = cur.next(); !p.empty() && kept < max; p = cur.next(), ++seen) {
      const char type = static_cast<char>(p[0]);
      const Locate loc = be::load16(p.data() + 1);
      if (type == 'R') {
        itch::Stock s;
        std::memcpy(s.data(), p.data() + 11, 8);
        if (wanted.contains(std::string(itch::trim(s)))) keep[loc] = true;
        write(p);  // keep the full directory so locate -> symbol stays resolvable
      } else if (loc == 0 || keep[loc]) {
        write(p);
      }
    }
  }
  std::fclose(out);
  std::printf("scanned %llu messages, wrote %llu\n", static_cast<unsigned long long>(seen),
              static_cast<unsigned long long>(kept));
  return 0;
}
