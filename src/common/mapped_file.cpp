#include "ironbook/common/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ib {

namespace {
[[noreturn]] void throw_errno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}
std::size_t page_size() {
  static const auto ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  return ps;
}
}  // namespace

MappedFile::MappedFile(const std::string& path, Advice advice) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw_errno("open " + path);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw_errno("fstat " + path);
  }
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ == 0) {
    ::close(fd);
    return;
  }
  void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) throw_errno("mmap " + path);
  data_ = static_cast<const std::uint8_t*>(p);
  const int adv = advice == Advice::Sequential ? MADV_SEQUENTIAL
                  : advice == Advice::Random   ? MADV_RANDOM
                                               : MADV_WILLNEED;
  ::madvise(p, size_, adv);
}

MappedFile::~MappedFile() { release(); }

MappedFile::MappedFile(MappedFile&& o) noexcept
    : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
  if (this != &o) {
    release();
    data_ = std::exchange(o.data_, nullptr);
    size_ = std::exchange(o.size_, 0);
  }
  return *this;
}

void MappedFile::release() noexcept {
  if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
  data_ = nullptr;
  size_ = 0;
}

void MappedFile::prefault() const {
  volatile std::uint8_t sink = 0;
  const std::size_t ps = page_size();
  for (std::size_t off = 0; off < size_; off += ps) sink = sink + data_[off];
  (void)sink;
}

VirtualArena::VirtualArena(std::size_t bytes) {
  const std::size_t ps = page_size();
  size_ = (bytes + ps - 1) / ps * ps;
  if (size_ == 0) return;
  void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) throw_errno("mmap arena");
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  // Transparent huge pages: one TLB entry covers 2 MiB of pool instead of 4 KiB. (macOS
  // offers no userspace superpages on arm64; its 16 KiB base pages already help.)
  ::madvise(p, size_, MADV_HUGEPAGE);
#endif
  data_ = p;
}

VirtualArena::~VirtualArena() { release(); }

VirtualArena::VirtualArena(VirtualArena&& o) noexcept
    : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}

VirtualArena& VirtualArena::operator=(VirtualArena&& o) noexcept {
  if (this != &o) {
    release();
    data_ = std::exchange(o.data_, nullptr);
    size_ = std::exchange(o.size_, 0);
  }
  return *this;
}

void VirtualArena::release() noexcept {
  if (data_) ::munmap(data_, size_);
  data_ = nullptr;
  size_ = 0;
}

bool lock_memory(const void* p, std::size_t bytes) {
  if (bytes == 0) return true;
  const std::size_t ps = page_size();
  const auto begin = reinterpret_cast<std::uintptr_t>(p) / ps * ps;
  const auto end = (reinterpret_cast<std::uintptr_t>(p) + bytes + ps - 1) / ps * ps;
  return ::mlock(reinterpret_cast<const void*>(begin), end - begin) == 0;
}

bool VirtualArena::lock(std::size_t bytes) { return lock_memory(data_, bytes < size_ ? bytes : size_); }

void VirtualArena::prefault(std::size_t bytes) {
  if (bytes > size_) bytes = size_;
  auto* p = static_cast<volatile std::uint8_t*>(data_);
  const std::size_t ps = page_size();
  for (std::size_t off = 0; off < bytes; off += ps) p[off] = 0;
}

}  // namespace ib
