#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ib {

// mlock helper for arbitrary memory (hash tables, buffers). Returns false on failure.
bool lock_memory(const void* p, std::size_t bytes);

// Read-only memory-mapped file. The ITCH parser runs directly over the mapping, so the
// replay path performs zero copies and zero read() syscalls.
class MappedFile {
 public:
  enum class Advice { Sequential, Random, WillNeed };

  MappedFile() = default;
  explicit MappedFile(const std::string& path, Advice advice = Advice::Sequential);
  ~MappedFile();

  MappedFile(MappedFile&& o) noexcept;
  MappedFile& operator=(MappedFile&& o) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  // Touch every page so later passes (benchmarks) do not pay for page faults.
  void prefault() const;

  const std::uint8_t* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  std::span<const std::uint8_t> bytes() const noexcept { return {data_, size_}; }
  explicit operator bool() const noexcept { return data_ != nullptr; }

 private:
  void release() noexcept;
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

// Anonymous virtual-memory reservation used to back object pools. Address space is
// reserved up front (so the pool never reallocates or moves); physical pages are
// committed by the OS on first touch, or eagerly via prefault().
class VirtualArena {
 public:
  VirtualArena() = default;
  explicit VirtualArena(std::size_t bytes);
  ~VirtualArena();

  VirtualArena(VirtualArena&& o) noexcept;
  VirtualArena& operator=(VirtualArena&& o) noexcept;
  VirtualArena(const VirtualArena&) = delete;
  VirtualArena& operator=(const VirtualArena&) = delete;

  void prefault(std::size_t bytes);
  // Wire the first `bytes` into RAM (mlock): no page faults, never swapped or compressed.
  bool lock(std::size_t bytes);
  void* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

 private:
  void release() noexcept;
  void* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace ib
