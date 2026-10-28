#include "order_book.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <simdjson.h>

using namespace feedguard;
namespace {
Instrument instrument() { return {1, 3, 5, 2, {}}; }
MarketEvent event(EventKind kind, std::uint64_t u = 2, std::uint64_t seq = 10, std::uint64_t session = 1) {
    MarketEvent e;
    e.kind = kind; e.session = session; e.update_id = u; e.sequence = seq;
    e.exchange_ms = 100; e.receive_utc_ns = 1000; e.receive_steady_ns = 100;
    if (kind == EventKind::reset) {
        e.update_id = 0; e.sequence = 0; e.exchange_ms = 0; e.receive_steady_ns = 0;
    }
    return e;
}
template<class Book> void apply(Book& book, MarketEvent e, std::span<const Level> bids = {}, std::span<const Level> asks = {}) {
    e.bid_count = static_cast<std::uint32_t>(bids.size());
    e.ask_count = static_cast<std::uint32_t>(asks.size());
    book.apply(e, bids, asks);
}
template<class Book> void initial(Book& book) {
    const std::array<Level, 2> bids{{{1005,4},{1000,2}}}, asks{{{1010,6},{1020,8}}};
    apply(book, event(EventKind::snapshot), bids, asks);
}
EventData golden() {
    const auto dir = std::filesystem::path(FEEDGUARD_FIXTURES);
    return convert_capture(dir / "capture-golden.jsonl", read_instrument(dir / "instrument-synthetic.json"));
}
}
TEMPLATE_TEST_CASE("Book snapshot builds sorted levels best quotes and exact spread", "", MapBook, VectorBook) {
    TestType book(instrument());
    REQUIRE_FALSE(book.valid());
    REQUIRE_FALSE(book.best_bid());
    REQUIRE_FALSE(book.spread());
    initial(book);
    REQUIRE(book.valid());
    REQUIRE(book.state().bids == std::vector<Level>{{1005,4},{1000,2}});
    REQUIRE(book.state().asks == std::vector<Level>{{1010,6},{1020,8}});
    REQUIRE(book.best_bid() == Level{1005,4});
    REQUIRE(book.best_ask() == Level{1010,6});
    REQUIRE(book.spread() == 5);
    REQUIRE(book.checksum() == 0xe674268449ae430cull);
}
TEMPLATE_TEST_CASE("Book delta inserts updates deletes and ignores absent zero levels", "", MapBook, VectorBook) {
    TestType book(instrument()); initial(book);
    const std::array<Level, 4> bids{{{995,12},{1005,10},{1000,0},{980,0}}};
    const std::array<Level, 3> asks{{{1010,0},{1015,14},{1020,16}}};
    apply(book, event(EventKind::delta, 3, 900), bids, asks);
    REQUIRE(book.state().bids == std::vector<Level>{{1005,10},{995,12}});
    REQUIRE(book.state().asks == std::vector<Level>{{1015,14},{1020,16}});
    REQUIRE(book.best_bid() == Level{1005,10});
    REQUIRE(book.best_ask() == Level{1015,14});
    REQUIRE(book.spread() == 10);
    REQUIRE(book.state().sequence == 900); // seq need not advance by one
}
TEMPLATE_TEST_CASE("New snapshot fully replaces both sides including empty sides", "", MapBook, VectorBook) {
    TestType book(instrument()); initial(book);
    const std::array<Level, 2> asks{{{1030,2},{1040,4}}};
    apply(book, event(EventKind::snapshot, 1, 950), {}, asks);
    REQUIRE(book.valid());
    REQUIRE(book.state().bids.empty());
    REQUIRE(book.state().asks == std::vector<Level>{{1030,2},{1040,4}});
    REQUIRE_FALSE(book.best_bid()); REQUIRE_FALSE(book.spread());
    apply(book, event(EventKind::snapshot, 2, 1000));
    REQUIRE(book.valid());
    REQUIRE(book.state().bids.empty()); REQUIRE(book.state().asks.empty());
    REQUIRE_FALSE(book.best_ask());
}
TEMPLATE_TEST_CASE("Reset invalidates quotes and preserves the last trustworthy state", "", MapBook, VectorBook) {
    TestType book(instrument()); initial(book);
    const auto before = book.state();
    apply(book, event(EventKind::reset));
    REQUIRE_FALSE(book.valid());
    REQUIRE(book.state().bids.empty()); REQUIRE(book.state().asks.empty());
    REQUIRE_FALSE(book.best_bid()); REQUIRE_FALSE(book.best_ask()); REQUIRE_FALSE(book.spread());
    REQUIRE(book.last_valid_before_reset() == before);
    apply(book, event(EventKind::reset, 0, 0, 2));
    REQUIRE(book.last_valid_before_reset() == before);
    REQUIRE_THROWS(apply(book, event(EventKind::delta, 3, 30, 2)));
    const std::array<Level, 1> bid{{{900,2}}};
    apply(book, event(EventKind::snapshot, 1, 40, 2), bid);
    REQUIRE(book.state().session == 2);
    REQUIRE(book.state().bids == std::vector<Level>{{900,2}});
    const auto second = book.state();
    apply(book, event(EventKind::reset, 0, 0, 2));
    REQUIRE(book.last_valid_before_reset() == second);
}
TEMPLATE_TEST_CASE("Rejected events leave book metadata levels checksum and archive unchanged", "", MapBook, VectorBook) {
    TestType book(instrument()); initial(book);
    apply(book, event(EventKind::reset));
    initial(book);
    const auto before = book.state();
    const auto checksum = book.checksum();
    const auto archived = book.last_valid_before_reset();
    REQUIRE(archived.has_value());
    auto reject = [&](MarketEvent e, std::vector<Level> bids, std::vector<Level> asks) {
        REQUIRE_THROWS(apply(book, e, bids, asks));
        REQUIRE(book.state() == before);
        REQUIRE(book.checksum() == checksum);
        REQUIRE(book.last_valid_before_reset() == archived);
    };
    reject(event(EventKind::delta), {{995,2}}, {{1015,-2}}); // late ASK error after a valid BID
    reject(event(EventKind::delta), {{995,2},{995,4}}, {});
    reject(event(EventKind::delta), {{996,2}}, {});
    reject(event(EventKind::delta), {{995,3}}, {});
    reject(event(EventKind::delta), {{0,2}}, {});
    reject(event(EventKind::delta), {{-5,2}}, {});
    reject(event(EventKind::snapshot), {{995,2},{1000,2}}, {});
    reject(event(EventKind::snapshot), {{995,2}}, {{1020,2},{1010,2}});
    reject(event(EventKind::snapshot), {{995,0}}, {});
    reject(event(EventKind::delta, 1), {{995,2}}, {});
    reject(event(EventKind::delta, 3, 30, 2), {{995,2}}, {});
    reject(event(EventKind::snapshot, 1, 30, 2), {{995,2}}, {});
    reject(event(static_cast<EventKind>(255)), {}, {});
    auto backwards = event(EventKind::delta); backwards.receive_steady_ns = 99;
    reject(backwards, {{995,2}}, {});
    auto no_time = event(EventKind::delta); no_time.exchange_ms = 0;
    reject(no_time, {}, {});
    auto future_bad = event(EventKind::delta); future_bad.receive_steady_ns = 1000000;
    reject(future_bad, {{995,2}}, {{1015,-2}});
    auto bad_reset = event(EventKind::reset); bad_reset.update_id = 1;
    reject(bad_reset, {}, {});
    auto bad_count = event(EventKind::delta); bad_count.bid_count = 2;
    const std::array<Level, 1> one{{{995,2}}};
    REQUIRE_THROWS(book.apply(bad_count, one, {}));
    REQUIRE(book.state() == before);
    std::vector<Level> too_many(4097, {995,2});
    reject(event(EventKind::delta), too_many, {});
    std::vector<Level> deep;
    for (std::int64_t i = 0; i < 51; ++i) deep.push_back({1000 - i * 5,2});
    reject(event(EventKind::snapshot), deep, {});
    auto continuation = event(EventKind::delta, 3, 1000); continuation.receive_steady_ns = 101;
    REQUIRE_NOTHROW(apply(book, continuation));
    REQUIRE(book.state().bids == before.bids); // rejected event did not advance hidden clock state
    REQUIRE(book.last_valid_before_reset() == archived);
}
TEMPLATE_TEST_CASE("Spread supports int64 price extremes without overflow", "", MapBook, VectorBook) {
    auto m = instrument(); m.tick_size = 1;
    TestType book(m);
    const auto max = std::numeric_limits<std::int64_t>::max();
    const std::array<Level, 1> bid{{{1,2}}}, ask{{{max,2}}};
    apply(book, event(EventKind::snapshot), bid, ask);
    REQUIRE(book.spread() == max - 1);
    apply(book, event(EventKind::snapshot), ask, bid);
    REQUIRE(book.spread() == 1 - max); // faithfully report crossed input, do not silently repair it
}
TEMPLATE_TEST_CASE("Golden replay matches every hand-calculated state and independent FNV checksum", "", MapBook, VectorBook) {
    const auto data = golden();
    const std::array<BookState, 7> expected{{
        {false,1,0,0,{},{}},
        {true,1,2,10,{{1005,4},{1000,2}},{{1010,6},{1020,8}}},
        {true,1,3,900,{{1000,10}},{{1010,6},{1015,2},{1020,8}}},
        {false,1,0,0,{},{}}, {false,2,0,0,{},{}},
        {true,2,1,950,{},{{1010,2},{1020,4}}}, {false,2,0,0,{},{}}
    }};
    const std::array<std::uint64_t, 7> hashes{{0x2e26fc092cefe0bcull,0xe674268449ae430cull,
        0x3eee8e4070cae004ull,0x2e26fc092cefe0bcull,0x09fb16688567b09full,
        0x70084ebfcaa3d624ull,0x09fb16688567b09full}};
    TestType book(data.instrument);
    for (std::size_t i = 0; i < data.events.size(); ++i) {
        INFO(i);
        const auto& e = data.events[i];
        book.apply(e, data.bids(e), data.asks(e));
        REQUIRE(book.state() == expected[i]);
        REQUIRE(book.checksum() == hashes[i]);
        REQUIRE(state_checksum(data.instrument, book.state()) == hashes[i]);
    }
    const auto replay = replay_map(data);
    REQUIRE(replay.events == 7); REQUIRE(replay.market_events == 3); REQUIRE(replay.resets == 4);
    REQUIRE(replay.checksum == hashes.back());
    REQUIRE(replay.history_checksum == 0x6abb994ed1892e86ull);
    REQUIRE(replay.last_valid_before_reset == expected[5]);
    simdjson::dom::parser parser;
    auto json = replay_json(replay);
    auto doc = parser.parse(json);
    REQUIRE(doc["checksum"].get_string().value() == "09fb16688567b09f");
    REQUIRE(doc["history_checksum"].get_string().value() == "6abb994ed1892e86");
    REQUIRE(doc["current"]["valid"].get_bool().value() == false);
    REQUIRE(doc["last_valid_before_reset"]["asks"].get_array().value().size() == 2);
}
TEST_CASE("FNV uses standard byte order and unsigned wrapping") {
    Fnv1a64 empty;
    REQUIRE(empty.value() == 0xcbf29ce484222325ull);
    Fnv1a64 hello; hello.bytes("hello");
    REQUIRE(hello.value() == 0xa430d84680aabd0bull);
    Fnv1a64 bytes, integer;
    for (auto c : {0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01}) bytes.byte(static_cast<std::uint8_t>(c));
    integer.integer(0x0102030405060708ull);
    REQUIRE(integer.value() == bytes.value());
    REQUIRE(hex_checksum(1) == "0000000000000001");
}
TEMPLATE_TEST_CASE("Book accepts int64 boundary levels and rejects invalid instrument steps", "", MapBook, VectorBook) {
    auto m = instrument(); m.tick_size = 1; m.quantity_step = 1;
    TestType book(m);
    const auto max = std::numeric_limits<std::int64_t>::max();
    const std::array<Level, 1> bids{{{max,max}}};
    apply(book, event(EventKind::snapshot), bids);
    REQUIRE(book.best_bid() == Level{max,max});
    const std::array<Level, 1> deleted{{{max,0}}};
    apply(book, event(EventKind::delta, 3), deleted);
    REQUIRE(book.state().bids.empty());
    m.tick_size = 0;
    REQUIRE_THROWS_AS(TestType(m), std::runtime_error);
    m.tick_size = 1; m.quantity_step = -1;
    REQUIRE_THROWS_AS(TestType(m), std::runtime_error);
    m.quantity_step = 1; m.price_decimals = 19;
    REQUIRE_THROWS_AS(TestType(m), std::runtime_error);
}
TEST_CASE("One hundred thousand book events match an independent vector oracle") {
    MapBook book(instrument());
    VectorBook optimized(instrument());
    BookState reference;
    std::optional<BookState> saved;
    std::mt19937_64 random(0xb17b00cull);
    std::uint64_t session = 0;
    auto update = [](std::vector<Level>& side, std::span<const Level> changes, bool bid) {
        for (auto level : changes) {
            auto found = std::find_if(side.begin(), side.end(), [&](auto x) { return x.price == level.price; });
            if (!level.quantity) { if (found != side.end()) side.erase(found); }
            else if (found == side.end()) side.push_back(level);
            else *found = level;
        }
        std::sort(side.begin(), side.end(), [bid](auto a, auto b) { return bid ? a.price > b.price : a.price < b.price; });
    };
    for (std::uint64_t i = 0; i < 100000; ++i) {
        INFO(i);
        MarketEvent e;
        std::vector<Level> bids, asks;
        if (i % 1000 == 0) {
            e = event(EventKind::reset, 0, 0, ++session);
            if (reference.valid) saved = reference;
            reference = {false,session,0,0,{},{}};
        } else {
            e = event(i % 1000 == 1 ? EventKind::snapshot : EventKind::delta,
                      i % 1000 == 1 ? 1 : i + 2, i * 17, session);
            e.receive_steady_ns = static_cast<std::int64_t>(i + 1000);
            if (e.kind == EventKind::snapshot) {
                bids = {{10000,2},{9995,4}}; asks = {{11000,6},{11005,8}};
                reference.bids.clear(); reference.asks.clear();
            } else {
                bids = {{10000 - static_cast<std::int64_t>(5 * (random() % 64)), static_cast<std::int64_t>(2 * (random() % 16))}};
                asks = {{11000 + static_cast<std::int64_t>(5 * (random() % 64)), static_cast<std::int64_t>(2 * (random() % 16))}};
            }
            update(reference.bids, bids, true); update(reference.asks, asks, false);
            reference.valid = true; reference.session = session;
            reference.update_id = e.update_id; reference.sequence = e.sequence;
        }
        apply(book, e, bids, asks);
        apply(optimized, e, bids, asks);
        REQUIRE(book.state() == reference);
        REQUIRE(optimized.state() == reference);
        REQUIRE(optimized.checksum() == book.checksum());
        REQUIRE(book.last_valid_before_reset() == saved);
        REQUIRE(optimized.last_valid_before_reset() == saved);
    }
}
TEST_CASE("History checksum catches intermediate states hidden by final reset") {
    auto original = golden();
    auto changed = original;
    changed.levels[0].quantity += 2;
    const auto a = replay_map(original), b = replay_map(changed);
    REQUIRE(a.current == b.current);
    REQUIRE(a.last_valid_before_reset == b.last_valid_before_reset);
    REQUIRE(a.checksum == b.checksum);
    REQUIRE(a.history_checksum != b.history_checksum);
}
TEST_CASE("Vector capacity grows without losing levels and is reused after reset") {
    MapBook baseline(instrument()); VectorBook optimized(instrument());
    auto compare = [&](MarketEvent e, const std::vector<Level>& bids, const std::vector<Level>& asks) {
        apply(baseline, e, bids, asks); apply(optimized, e, bids, asks);
        REQUIRE(optimized.state() == baseline.state());
        REQUIRE(optimized.checksum() == baseline.checksum());
        REQUIRE(optimized.last_valid_before_reset() == baseline.last_valid_before_reset());
    };
    compare(event(EventKind::snapshot), {}, {});
    REQUIRE(optimized.bid_capacity() >= 64);
    std::vector<Level> bids, asks;
    for (std::int64_t i = 0; i < 4096; ++i) {
        bids.push_back({100000 - 5*i, 2}); asks.push_back({110000 + 5*i, 4});
    }
    compare(event(EventKind::delta, 3), bids, asks);
    REQUIRE(optimized.state().bids.size() == 4096);
    REQUIRE(optimized.state().asks.size() == 4096);
    const auto bc = optimized.bid_capacity(), ac = optimized.ask_capacity();
    compare(event(EventKind::delta, 4), {{100005,2}}, {{109995,4}});
    REQUIRE(optimized.state().bids.size() == 4097);
    REQUIRE(optimized.bid_capacity() > bc); REQUIRE(optimized.ask_capacity() > ac);
    const auto grown = optimized.bid_capacity();
    // All new prices are supplied before deletions; final size is unchanged.
    compare(event(EventKind::delta, 5), {{100010,2},{100005,0}}, {{109990,2},{109995,0}});
    REQUIRE(optimized.bid_capacity() == grown);
    compare(event(EventKind::reset), {}, {});
    REQUIRE(optimized.bid_capacity() == grown);
    compare(event(EventKind::snapshot, 1), {{1000,2}}, {});
    REQUIRE(optimized.bid_capacity() == grown);
}
TEST_CASE("Map and vector replay agree on golden data and benchmark counts are exact") {
    const auto data = golden();
    const auto expected = replay_map(data);
    REQUIRE(replay_vector(data) == expected);
    REQUIRE(verify_engines(data) == expected);
    for (auto engine : {Engine::map, Engine::vector}) {
        const auto report = benchmark_core(data, engine, 2);
        REQUIRE(report.source_events == 7);
        REQUIRE(report.total_events == 14);
        REQUIRE(report.source_market_events == 3);
        REQUIRE(report.checksum == expected.checksum);
        REQUIRE(report.verified_history_checksum == expected.history_checksum);
        REQUIRE(report.last_valid_checksum == 0x70084ebfcaa3d624ull);
        auto json = benchmark_json(report);
        simdjson::dom::parser parser;
        REQUIRE(parser.parse(json)["checksum_in_timed_loop"].get_bool().value() == false);
    }
    REQUIRE_THROWS(benchmark_core(data, Engine::map, 0));
    REQUIRE_THROWS(benchmark_core(data, Engine::map, 1001));
}
