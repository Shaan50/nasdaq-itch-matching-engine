#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "ironbook/common/mapped_file.hpp"
#include "ironbook/itch/parser.hpp"

namespace ib::itch {

bool is_gzip_path(const std::string& path);

// Streams a gzip-compressed ITCH file through zlib in large chunks. `consume` receives
// the currently buffered bytes, reports how many it used via `used` (a partial trailing
// frame is carried into the next chunk), and returns false to stop early.
void stream_gzip(const std::string& path,
                 const std::function<bool(std::span<const std::uint8_t>, std::size_t& used)>& consume,
                 std::size_t chunk_bytes = 8u << 20);

// Replays an ITCH file (raw: mmap, zero-copy; .gz: streaming inflate) into handler `h`.
template <class H>
ParseStats replay_file(const std::string& path, H& h,
                       std::uint64_t max_messages = ~std::uint64_t{0}) {
  ParseStats stats;
  if (is_gzip_path(path)) {
    stream_gzip(path, [&](std::span<const std::uint8_t> buf, std::size_t& used) {
      used = parse_frames(buf, h, stats, max_messages - stats.messages);
      return stats.messages < max_messages;
    });
  } else {
    MappedFile file(path);
    parse_frames(file.bytes(), h, stats, max_messages);
  }
  return stats;
}

}  // namespace ib::itch
