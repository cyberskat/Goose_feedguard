#include "collector.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>

using namespace feedguard;
namespace {
std::string market(std::string_view type = "snapshot", std::string_view price = "65000.10") {
    return "{\"topic\":\"orderbook.50.BTCUSDT\",\"type\":\"" + std::string(type) +
        "\",\"ts\":1,\"data\":{\"s\":\"BTCUSDT\",\"u\":2,\"seq\":9,\"b\":[[\"" +
        std::string(price) + "\",\"0.001\"]],\"a\":[]}}";
}
}
TEST_CASE("Collector accepts only valid market envelopes") {
    simdjson::dom::parser parser;
    REQUIRE(classify(parser, market()) == MessageKind::snapshot);
    REQUIRE(classify(parser, market("delta")) == MessageKind::delta);
    REQUIRE(classify(parser, market("unknown")) == MessageKind::invalid);
    for (auto price : {"", ".5", "5.", "NaN", "1e3", "-1", "1.2.3"})
        REQUIRE(classify(parser, market("delta", price)) == MessageKind::invalid);
    REQUIRE(classify(parser, "{") == MessageKind::invalid);
    REQUIRE(classify(parser, "[]") == MessageKind::invalid);
    REQUIRE(classify(parser, R"({"op":"ping","success":true})") == MessageKind::control);
    REQUIRE(classify(parser, R"({"op":"subscribe","success":false})") == MessageKind::rejected);
    auto bad = market();
    bad.replace(bad.find("\"u\":2"), 5, "\"u\":-1");
    REQUIRE(classify(parser, bad) == MessageKind::invalid);
    auto restart = market();
    restart.replace(restart.find("\"u\":2"), 5, "\"u\":1");
    REQUIRE(classify(parser, restart) == MessageKind::snapshot);
    restart.replace(restart.find("snapshot"), 8, "delta");
    REQUIRE(classify(parser, restart) == MessageKind::invalid);
    auto overflow = market();
    overflow.replace(overflow.find("\"u\":2"), 5, "\"u\":18446744073709551616");
    REQUIRE(classify(parser, overflow) == MessageKind::invalid);
}
TEST_CASE("Reconnect requires snapshot before any delta") {
    SessionGate gate;
    REQUIRE_FALSE(gate.accept(MessageKind::delta));
    REQUIRE(gate.accept(MessageKind::snapshot));
    REQUIRE(gate.accept(MessageKind::delta));
    gate.reset();
    REQUIRE_FALSE(gate.accept(MessageKind::delta));
    REQUIRE_FALSE(gate.accept(MessageKind::invalid));
    REQUIRE_FALSE(gate.accept(MessageKind::control));
    REQUIRE(gate.accept(MessageKind::snapshot));
}
TEST_CASE("Reconnect backoff is exponential and capped") {
    REQUIRE(backoff_seconds(0) == 1);
    REQUIRE(backoff_seconds(1) == 2);
    REQUIRE(backoff_seconds(4) == 16);
    REQUIRE(backoff_seconds(5) == 30);
    REQUIRE(backoff_seconds(1000) == 30);
}
TEST_CASE("Journal preserves payload and never overwrites existing output") {
    auto path = std::filesystem::temp_directory_path() /
        ("feedguard-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove(p, ec); } } cleanup{path};
    Journal journal(path);
    REQUIRE_THROWS_AS(Journal(path), std::runtime_error);
    journal.message(7, "snapshot", market(), 123, 456, true);
    journal.event(7, "disconnect", "quoted\"\nreason");
    journal.finish();
    simdjson::dom::parser parser;
    std::ifstream input(path);
    std::string line;
    REQUIRE(static_cast<bool>(std::getline(input, line)));
    auto doc = parser.parse(line);
    REQUIRE(doc["session"].get_uint64().value() == 7);
    REQUIRE(doc["accepted"].get_bool().value());
    REQUIRE(doc["raw"]["data"]["b"].at(0).at(0).get_string().value() == "65000.10");
    REQUIRE(doc["recv_steady_ns"].get_int64().value() == 456);
    REQUIRE(static_cast<bool>(std::getline(input, line)));
    REQUIRE(parser.parse(line)["detail"].get_string().value() == "quoted\"\nreason");
    REQUIRE_FALSE(static_cast<bool>(std::getline(input, line)));
}
TEST_CASE("Journal keeps multiline JSON on one physical line") {
    auto path = std::filesystem::temp_directory_path() /
        ("feedguard-multiline-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove(p, ec); } } cleanup{path};
    Journal journal(path);
    journal.message(1, "control", "{\n\"op\":\"ping\",\r\n\"text\":\"escaped\\nnewline\"\n}", 1, 1, false);
    journal.finish();
    std::ifstream input(path);
    std::string line;
    REQUIRE(static_cast<bool>(std::getline(input, line)));
    simdjson::dom::parser parser;
    REQUIRE(parser.parse(line)["raw"]["text"].get_string().value() == "escaped\nnewline");
    REQUIRE_FALSE(static_cast<bool>(std::getline(input, line)));
}
TEST_CASE("CLI rejects missing negative overflowing or unknown arguments") {
    auto parse = [](std::initializer_list<const char*> args) {
        std::vector<char*> values;
        for (auto value : args) values.push_back(const_cast<char*>(value));
        return parse_options(static_cast<int>(values.size()), values.data());
    };
    REQUIRE(parse({"collector", "--count", "12", "--timeout", "2"}).count == 12);
    REQUIRE_THROWS(parse({"collector", "--count"}));
    REQUIRE_THROWS(parse({"collector", "--count", "-1"}));
    REQUIRE_THROWS(parse({"collector", "--count", "0"}));
    REQUIRE_THROWS(parse({"collector", "--count", "18446744073709551616"}));
    REQUIRE_THROWS(parse({"collector", "--timeout", "86401"}));
    REQUIRE_THROWS(parse({"collector", "--unknown", "1"}));
}
