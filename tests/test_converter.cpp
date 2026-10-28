#include "market_event.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <chrono>
#include <tuple>

using namespace feedguard;
namespace {
const auto fixtures = std::filesystem::path(FEEDGUARD_FIXTURES);
struct Temporary {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("feedguard-convert-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temporary() { std::error_code ec; std::filesystem::remove(path, ec); }
    void text(std::string_view value) {
        std::ofstream f(path, std::ios::binary); f << value;
        if (!f) throw std::runtime_error("Cannot write test fixture");
    }
};
std::string read_text(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open fixture");
    return {std::istreambuf_iterator<char>(f), {}};
}
EventData golden() {
    return convert_capture(fixtures / "capture-golden.jsonl", read_instrument(fixtures / "instrument-synthetic.json"));
}
std::vector<std::uint8_t> golden_bytes() {
    std::istringstream text(read_text(fixtures / "events-v1-golden.hex"));
    std::vector<std::uint8_t> bytes;
    unsigned value;
    while (text >> std::hex >> value) bytes.push_back(static_cast<std::uint8_t>(value));
    return bytes;
}
void patch(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
void repair_checksums(std::vector<std::uint8_t>& bytes) {
    patch(bytes, 64, crc32(std::span<const std::uint8_t>(bytes).subspan(128)), 4);
    patch(bytes, 68, 0, 4);
    patch(bytes, 68, crc32(std::span<const std::uint8_t>(bytes).first(128)), 4);
}
}
TEST_CASE("Decimal conversion is exact at int64 boundaries") {
    REQUIRE(decimal_to_int64("80631.90", 1) == 806319);
    REQUIRE(decimal_to_int64("0.001", 3) == 1);
    REQUIRE(decimal_to_int64("00012.34000", 2) == 1234);
    REQUIRE(decimal_to_int64("1", 3) == 1000);
    REQUIRE(decimal_to_int64("-1.20", 2) == -120);
    REQUIRE(decimal_to_int64("9223372036854775807", 0) == std::numeric_limits<std::int64_t>::max());
    REQUIRE(decimal_to_int64("-9223372036854775808", 0) == std::numeric_limits<std::int64_t>::min());
    REQUIRE(decimal_to_int64("92233720368547758.07", 2) == std::numeric_limits<std::int64_t>::max());
    REQUIRE(decimal_to_int64("-92233720368547758.08", 2) == std::numeric_limits<std::int64_t>::min());
    for (auto bad : {"", "-", "+1", "1.", ".1", "1e2", "NaN", " 1", "1.2.3", "--1", "1x"})
        REQUIRE_THROWS(decimal_to_int64(bad, 2));
    REQUIRE_THROWS(decimal_to_int64("0.001", 2));
    REQUIRE_THROWS(decimal_to_int64("9223372036854775808", 0));
    REQUIRE_THROWS(decimal_to_int64("-9223372036854775809", 0));
    REQUIRE_THROWS(decimal_to_int64("9223372036854775807", 1));
    REQUIRE_THROWS(decimal_to_int64("0", 19));
}
TEST_CASE("Instrument steps come from metadata and support fractional ticks") {
    auto m = read_instrument(fixtures / "instrument-synthetic.json");
    REQUIRE(m.price_decimals == 1);
    REQUIRE(m.tick_size == 5);
    REQUIRE(m.quantity_decimals == 3);
    REQUIRE(m.quantity_step == 2);
    const auto original = read_text(fixtures / "instrument-synthetic.json");
    for (auto bad : {"0", "-0.5", "1.", "1..0", "NaN", "0.0000000000000000001"}) {
        auto text = original;
        text.replace(text.find("0.50"), 4, bad);
        Temporary tmp; tmp.text(text);
        REQUIRE_THROWS(read_instrument(tmp.path));
    }
    Temporary error;
    error.text("{\"retCode\":10001}");
    REQUIRE_THROWS(read_instrument(error.path));
}
TEST_CASE("Golden capture has hand-calculated integers and session resets") {
    const auto data = golden();
    REQUIRE(data.events.size() == 7);
    REQUIRE(data.levels == std::vector<Level>{{1005,4},{1000,2},{1010,6},{1020,8},
        {1005,0},{1000,10},{1015,2},{1010,2},{1020,4}});
    REQUIRE(data.events[0].kind == EventKind::reset);
    REQUIRE(data.events[1].exchange_ms == 12345);
    REQUIRE(data.events[1].matching_ms == 12344);
    REQUIRE(data.events[2].sequence == 900);
    REQUIRE(data.events[2].matching_ms == 0);
    REQUIRE(data.events[5].session == 2);
    REQUIRE(data.events[5].update_id == 1);
    REQUIRE(data.events[5].bid_count == 0);
    REQUIRE(data.asks(data.events[5]).size() == 2);
    REQUIRE(data.events.back().kind == EventKind::reset);
    REQUIRE(encode_events(data) == golden_bytes());
    REQUIRE(decode_events(golden_bytes()) == data);
}
TEST_CASE("CRC32 matches the published standard check vector") {
    const std::string text = "123456789";
    REQUIRE(crc32({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}) == 0xcbf43926u);
}
TEST_CASE("Reader rejects truncation corruption trailing bytes and forged lengths") {
    const auto original = golden_bytes();
    for (std::size_t n = 0; n < original.size(); ++n)
        REQUIRE_THROWS(decode_events(std::span<const std::uint8_t>(original).first(n)));
    for (std::size_t n = 0; n < original.size(); ++n) {
        auto bytes = original; bytes[n] ^= 1;
        REQUIRE_THROWS(decode_events(bytes));
    }
    auto trailing = original; trailing.push_back(0);
    REQUIRE_THROWS(decode_events(trailing));
    // Repair CRCs to reach structural/semantic checks, rather than testing only CRC.
    for (auto [offset, value, width] : std::vector<std::tuple<std::size_t, std::uint64_t, unsigned>>{
        {8,2,4}, {16,19,4}, {24,0,8}, {40,~0ull,8}, {48,99,8},
        {128,~0u,4}, {132,9,1}, {133,1,1}, {128+72+64,4097,4},
        {128+72+16,0,8}, {128+72+72,1006,8}, {128+72+72+8,0,8}}) {
        auto bytes = original; patch(bytes, offset, value, width); repair_checksums(bytes);
        REQUIRE_THROWS(decode_events(bytes));
    }
}
TEST_CASE("Converter rejects malformed decimals off-grid data and session violations") {
    const auto source = read_text(fixtures / "capture-golden.jsonl");
    const auto m = read_instrument(fixtures / "instrument-synthetic.json");
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
        {"100.50", "100.55"}, {"0.004", "0.003"}, {"0.004", "-0.004"},
        {"0.004", "0"}, {"100.50", "9223372036854775808"},
        {"\"accepted\":true", "\"accepted\":false"},
        {"\"seq\":10", "\"seq\":-1"}, {"\"cts\":12344", "\"cts\":null"},
        {"\"kind\":\"disconnect\"", "\"kind\":\"invalid\""},
        {"\"session\":2,\"kind\":\"snapshot\"", "\"session\":1,\"kind\":\"snapshot\""}}) {
        auto text = source;
        REQUIRE(text.find(from) != std::string::npos);
        text.replace(text.find(from), from.size(), to);
        Temporary tmp; tmp.text(text);
        REQUIRE_THROWS(convert_capture(tmp.path, m));
    }
    Temporary incomplete;
    incomplete.text(source.substr(0, source.rfind("{\"schema\"")));
    REQUIRE_THROWS(convert_capture(incomplete.path, m));
    Temporary garbage; garbage.text(source + "{}\n");
    REQUIRE_THROWS(convert_capture(garbage.path, m));
    Temporary overlong; overlong.text(std::string(2 * 1024 * 1024 + 2, 'x'));
    REQUIRE_THROWS(convert_capture(overlong.path, m));
    // A legal maximum-sized raw payload plus its envelope must still fit.
    auto padded = source;
    padded.insert(padded.find("\"raw\":{") + 7, 1024 * 1024, ' ');
    Temporary large_valid; large_valid.text(padded);
    REQUIRE(convert_capture(large_valid.path, m) == golden());
}
TEST_CASE("Binary round trip is exact and existing files are not overwritten") {
    Temporary tmp;
    const auto data = golden();
    write_events(tmp.path, data);
    REQUIRE(read_events(tmp.path) == data);
    const auto before = read_text(tmp.path);
    REQUIRE_THROWS(write_events(tmp.path, data));
    REQUIRE(read_text(tmp.path) == before);
}
TEST_CASE("One hundred thousand synthetic updates round trip without loss") {
    const auto source = golden();
    EventData data;
    data.instrument = source.instrument;
    data.events.reserve(100003);
    data.levels.reserve(100004);
    data.events.push_back(source.events[0]);
    data.events.push_back(source.events[1]);
    data.levels.insert(data.levels.end(), source.levels.begin(), source.levels.begin() + 4);
    for (std::uint64_t i = 0; i < 100000; ++i) {
        MarketEvent e;
        e.kind = EventKind::delta; e.session = 1; e.update_id = i + 3;
        e.sequence = 1000 + i * 17; e.exchange_ms = 12346 + i;
        e.receive_utc_ns = static_cast<std::int64_t>(102 + i);
        e.receive_steady_ns = static_cast<std::int64_t>(202 + i);
        e.first_level = data.levels.size();
        if (i % 2) e.ask_count = 1; else e.bid_count = 1;
        data.levels.push_back({i % 2 ? 1010 : 1005, static_cast<std::int64_t>(2 * (i % 100))});
        data.events.push_back(e);
    }
    MarketEvent end;
    end.kind = EventKind::reset; end.session = 1; end.receive_utc_ns = 100103;
    end.first_level = data.levels.size(); data.events.push_back(end);
    REQUIRE(data.events.size() == 100003);
    const auto bytes = encode_events(data);
    REQUIRE(decode_events(bytes) == data);
}
