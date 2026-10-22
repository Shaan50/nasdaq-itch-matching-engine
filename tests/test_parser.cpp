#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include "ironbook/itch/encoder.hpp"
#include "ironbook/itch/file_reader.hpp"
#include "ironbook/itch/parser.hpp"

using namespace ib;
using namespace ib::itch;

namespace {

using AnyMsg = std::variant<AddOrder, OrderExecuted, OrderExecutedWithPrice, OrderCancel, OrderDelete,
                            OrderReplace, Trade, CrossTrade, BrokenTrade, SystemEvent, StockDirectory,
                            StockTradingAction>;

struct Recorder {
  std::vector<AnyMsg> msgs;
  void on(const AddOrder& m) { msgs.emplace_back(m); }
  void on(const OrderExecuted& m) { msgs.emplace_back(m); }
  void on(const OrderExecutedWithPrice& m) { msgs.emplace_back(m); }
  void on(const OrderCancel& m) { msgs.emplace_back(m); }
  void on(const OrderDelete& m) { msgs.emplace_back(m); }
  void on(const OrderReplace& m) { msgs.emplace_back(m); }
  void on(const Trade& m) { msgs.emplace_back(m); }
  void on(const CrossTrade& m) { msgs.emplace_back(m); }
  void on(const BrokenTrade& m) { msgs.emplace_back(m); }
  void on(const SystemEvent& m) { msgs.emplace_back(m); }
  void on(const StockDirectory& m) { msgs.emplace_back(m); }
  void on(const StockTradingAction& m) { msgs.emplace_back(m); }
};

constexpr Timestamp kTs = 34'200'123'456'789ull;  // 09:30:00.123456789

}  // namespace

TEST(Parser, MessageLengthsMatchSpec) {
  EXPECT_EQ(kMessageLength['A'], 36);
  EXPECT_EQ(kMessageLength['F'], 40);
  EXPECT_EQ(kMessageLength['E'], 31);
  EXPECT_EQ(kMessageLength['C'], 36);
  EXPECT_EQ(kMessageLength['X'], 23);
  EXPECT_EQ(kMessageLength['D'], 19);
  EXPECT_EQ(kMessageLength['U'], 35);
  EXPECT_EQ(kMessageLength['P'], 44);
  EXPECT_EQ(kMessageLength['Q'], 40);
  EXPECT_EQ(kMessageLength['R'], 39);
  EXPECT_EQ(kMessageLength['S'], 12);
  EXPECT_EQ(kMessageLength['I'], 50);
}

TEST(Parser, RoundTripsEveryOrderBookMessage) {
  Encoder enc;
  enc.tracking = 7;
  enc.add(42, kTs, 1001, Side::Buy, 300, "AAPL", 2'901'500);
  enc.add_mpid(42, kTs + 1, 1002, Side::Sell, 100, "AAPL", 2'902'000, "GSCO");
  enc.executed(42, kTs + 2, 1001, 100, 555);
  enc.executed_price(42, kTs + 3, 1002, 50, 556, true, 2'901'900);
  enc.cancel(42, kTs + 4, 1001, 25);
  enc.replace(42, kTs + 5, 1001, 1003, 400, 2'901'000);
  enc.del(42, kTs + 6, 1003);
  enc.trade(42, kTs + 7, 0, Side::Buy, 77, "AAPL", 2'901'700, 557);
  enc.cross(42, kTs + 8, 123456789012ull, "AAPL", 2'900'000, 558, 'O');
  enc.broken(42, kTs + 9, 556);

  Recorder rec;
  ParseStats st;
  const auto consumed = parse_frames(std::span(enc.bytes()), rec, st);
  EXPECT_EQ(consumed, enc.size());
  ASSERT_EQ(rec.msgs.size(), 10u);
  EXPECT_EQ(st.messages, 10u);
  EXPECT_EQ(st.unknown + st.bad_length, 0u);

  const auto& a = std::get<AddOrder>(rec.msgs[0]);
  EXPECT_EQ(a.h.locate, 42);
  EXPECT_EQ(a.h.tracking, 7);
  EXPECT_EQ(a.h.timestamp, kTs);
  EXPECT_EQ(a.ref, 1001u);
  EXPECT_EQ(a.side, Side::Buy);
  EXPECT_EQ(a.shares, 300u);
  EXPECT_EQ(trim(a.stock), "AAPL");
  EXPECT_EQ(a.price, 2'901'500u);
  EXPECT_FALSE(a.has_mpid);

  const auto& f = std::get<AddOrder>(rec.msgs[1]);
  EXPECT_TRUE(f.has_mpid);
  EXPECT_EQ(std::string(f.mpid.data(), 4), "GSCO");
  EXPECT_EQ(f.side, Side::Sell);

  const auto& e = std::get<OrderExecuted>(rec.msgs[2]);
  EXPECT_EQ(e.ref, 1001u);
  EXPECT_EQ(e.shares, 100u);
  EXPECT_EQ(e.match, 555u);

  const auto& c = std::get<OrderExecutedWithPrice>(rec.msgs[3]);
  EXPECT_TRUE(c.printable);
  EXPECT_EQ(c.price, 2'901'900u);
  EXPECT_EQ(c.match, 556u);

  EXPECT_EQ(std::get<OrderCancel>(rec.msgs[4]).shares, 25u);
  const auto& u = std::get<OrderReplace>(rec.msgs[5]);
  EXPECT_EQ(u.orig_ref, 1001u);
  EXPECT_EQ(u.new_ref, 1003u);
  EXPECT_EQ(u.shares, 400u);
  EXPECT_EQ(u.price, 2'901'000u);
  EXPECT_EQ(std::get<OrderDelete>(rec.msgs[6]).ref, 1003u);
  EXPECT_EQ(std::get<Trade>(rec.msgs[7]).match, 557u);
  const auto& q = std::get<CrossTrade>(rec.msgs[8]);
  EXPECT_EQ(q.shares, 123456789012ull);
  EXPECT_EQ(q.cross_type, 'O');
  EXPECT_EQ(std::get<BrokenTrade>(rec.msgs[9]).match, 556u);
  EXPECT_EQ(std::get<BrokenTrade>(rec.msgs[9]).h.timestamp, kTs + 9);
}

TEST(Parser, ReferenceDataMessages) {
  Encoder enc;
  enc.system_event(1000, 'O');
  enc.stock_directory(17, 2000, "MSFT", 100);
  enc.trading_action(17, 3000, "MSFT", 'T');
  Recorder rec;
  ParseStats st;
  parse_frames(std::span(enc.bytes()), rec, st);
  ASSERT_EQ(rec.msgs.size(), 3u);
  EXPECT_EQ(std::get<SystemEvent>(rec.msgs[0]).event_code, 'O');
  const auto& r = std::get<StockDirectory>(rec.msgs[1]);
  EXPECT_EQ(trim(r.stock), "MSFT");
  EXPECT_EQ(r.round_lot_size, 100u);
  EXPECT_EQ(r.h.locate, 17);
  EXPECT_EQ(std::get<StockTradingAction>(rec.msgs[2]).trading_state, 'T');
}

TEST(Parser, MaxTimestampAndPriceSurvive) {
  Encoder enc;
  const Timestamp max48 = (1ull << 48) - 1;
  enc.add(65535, max48, ~0ull, Side::Sell, ~0u, "ZZZZZZZZ", ~0u);
  Recorder rec;
  ParseStats st;
  parse_frames(std::span(enc.bytes()), rec, st);
  const auto& a = std::get<AddOrder>(rec.msgs.at(0));
  EXPECT_EQ(a.h.timestamp, max48);
  EXPECT_EQ(a.h.locate, 65535);
  EXPECT_EQ(a.ref, ~0ull);
  EXPECT_EQ(a.shares, ~0u);
  EXPECT_EQ(a.price, ~0u);
  EXPECT_EQ(trim(a.stock), "ZZZZZZZZ");
}

TEST(Parser, StopsAtPartialFrameAndResumes) {
  Encoder enc;
  for (OrderId i = 1; i <= 100; ++i) enc.add(1, i, i, Side::Buy, 100, "X", 10000);
  const auto& bytes = enc.bytes();
  // Feed the stream in awkward chunk sizes, carrying unconsumed bytes forward.
  Recorder rec;
  ParseStats st;
  std::vector<std::uint8_t> carry;
  for (std::size_t off = 0; off < bytes.size(); off += 13) {
    const std::size_t n = std::min<std::size_t>(13, bytes.size() - off);
    carry.insert(carry.end(), bytes.begin() + off, bytes.begin() + off + n);
    const std::size_t used = parse_frames(std::span(carry), rec, st);
    carry.erase(carry.begin(), carry.begin() + used);
  }
  EXPECT_TRUE(carry.empty());
  ASSERT_EQ(rec.msgs.size(), 100u);
  for (OrderId i = 1; i <= 100; ++i) EXPECT_EQ(std::get<AddOrder>(rec.msgs[i - 1]).ref, i);
}

TEST(Parser, CountsUnknownAndTruncatedMessagesWithoutCrashing) {
  Encoder enc;
  const std::uint8_t unknown[5] = {'z', 0, 1, 0, 0};
  enc.raw(unknown, sizeof unknown);
  const std::uint8_t short_add[10] = {'A', 0, 1, 0, 0, 0, 0, 0, 0, 0};
  enc.raw(short_add, sizeof short_add);
  enc.del(1, 5, 99);
  Recorder rec;
  ParseStats st;
  parse_frames(std::span(enc.bytes()), rec, st);
  EXPECT_EQ(st.unknown, 1u);
  EXPECT_EQ(st.bad_length, 1u);
  ASSERT_EQ(rec.msgs.size(), 1u);
  EXPECT_EQ(std::get<OrderDelete>(rec.msgs[0]).ref, 99u);
}

TEST(Parser, MaxMessagesLimit) {
  Encoder enc;
  for (OrderId i = 1; i <= 10; ++i) enc.del(1, i, i);
  Recorder rec;
  ParseStats st;
  const auto used = parse_frames(std::span(enc.bytes()), rec, st, 4);
  EXPECT_EQ(rec.msgs.size(), 4u);
  EXPECT_EQ(used, 4u * (2 + 19));
}

TEST(Parser, HandlerWithoutOverloadsCompilesAndIgnores) {
  struct OnlyDeletes {
    int n = 0;
    void on(const OrderDelete&) { ++n; }
  } h;
  Encoder enc;
  enc.add(1, 1, 1, Side::Buy, 1, "A", 1);
  enc.del(1, 2, 1);
  ParseStats st;
  parse_frames(std::span(enc.bytes()), h, st);
  EXPECT_EQ(h.n, 1);
  EXPECT_EQ(st.by_type['A'], 1u);
  EXPECT_EQ(st.by_type['D'], 1u);
}

TEST(FileReader, GzipAndRawProduceIdenticalStreams) {
  Encoder enc;
  for (OrderId i = 1; i <= 5000; ++i) enc.add(3, i, i, i % 2 ? Side::Buy : Side::Sell, 100, "T", 10000 + i);
  const std::string raw = ::testing::TempDir() + "ib_feed.itch";
  const std::string gz = raw + ".gz";
  {
    FILE* f = std::fopen(raw.c_str(), "wb");
    std::fwrite(enc.bytes().data(), 1, enc.size(), f);
    std::fclose(f);
    gzFile g = gzopen(gz.c_str(), "wb");
    gzwrite(g, enc.bytes().data(), static_cast<unsigned>(enc.size()));
    gzclose(g);
  }
  Recorder a, b;
  const auto sa = replay_file(raw, a);
  // Tiny chunks force many partial frames across inflate boundaries.
  ParseStats sb;
  stream_gzip(gz, [&](std::span<const std::uint8_t> buf, std::size_t& used) {
    used = parse_frames(buf, b, sb);
    return true;
  }, 1000);
  EXPECT_EQ(sa.messages, 5000u);
  EXPECT_EQ(sb.messages, 5000u);
  ASSERT_EQ(a.msgs.size(), b.msgs.size());
  for (std::size_t i = 0; i < a.msgs.size(); ++i)
    EXPECT_EQ(std::get<AddOrder>(a.msgs[i]).price, std::get<AddOrder>(b.msgs[i]).price);
  std::remove(raw.c_str());
  std::remove(gz.c_str());
}
