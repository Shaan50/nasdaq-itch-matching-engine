#include "ironbook/itch/file_reader.hpp"

#include <zlib.h>

#include <cstring>
#include <stdexcept>
#include <vector>

namespace ib::itch {

bool is_gzip_path(const std::string& path) {
  return path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
}

void stream_gzip(const std::string& path,
                 const std::function<bool(std::span<const std::uint8_t>, std::size_t& used)>& consume,
                 std::size_t chunk_bytes) {
  gzFile gz = gzopen(path.c_str(), "rb");
  if (!gz) throw std::runtime_error("gzopen failed: " + path);
  gzbuffer(gz, 1u << 20);

  std::vector<std::uint8_t> buf(chunk_bytes + 65536);
  std::size_t have = 0;  // carried-over bytes at the front of buf
  for (;;) {
    const int n = gzread(gz, buf.data() + have, static_cast<unsigned>(buf.size() - have));
    if (n < 0) {
      int err = 0;
      const char* msg = gzerror(gz, &err);
      gzclose(gz);
      throw std::runtime_error(std::string("gzread: ") + msg);
    }
    have += static_cast<std::size_t>(n);
    if (have == 0) break;
    std::size_t used = 0;
    const bool more = consume({buf.data(), have}, used);
    std::memmove(buf.data(), buf.data() + used, have - used);
    have -= used;
    if (!more || n == 0) break;
  }
  gzclose(gz);
}

}  // namespace ib::itch
