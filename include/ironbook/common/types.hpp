#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ib {

using OrderId = std::uint64_t;
using Price = std::uint32_t;      // ITCH Price(4): 4 implied decimal places
using Qty = std::uint32_t;        // shares
using Locate = std::uint16_t;     // ITCH stock locate code, dense per day
using Timestamp = std::uint64_t;  // nanoseconds since midnight

inline constexpr Price kPriceScale = 10'000;
inline constexpr Price kMaxPrice = 0xFFFF'FFFFu;
inline constexpr std::size_t kMaxLocates = 1u << 16;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr int side_index(Side s) noexcept { return static_cast<int>(s); }
constexpr char side_char(Side s) noexcept { return s == Side::Buy ? 'B' : 'S'; }

// Fixed-point price -> human readable "123.4500".
std::string format_price(Price p);

inline double to_double(Price p) noexcept { return static_cast<double>(p) / kPriceScale; }

}  // namespace ib
