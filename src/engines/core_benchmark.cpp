#include "order_book.hpp"
#include <chrono>
#include <limits>
#include <stdexcept>

namespace feedguard {
namespace {
template<class Book> CoreBenchmark measure(const EventData& data, Engine engine,
                                           std::uint64_t repeats, const ReplayResult& expected) {
    std::vector<Book> books;
    books.reserve(static_cast<std::size_t>(repeats));
    for (std::uint64_t i = 0; i < repeats; ++i) books.emplace_back(data.instrument);
    // Every replay has a fresh clock/session; preparation and destruction are untimed.
    const auto start = std::chrono::steady_clock::now();
    for (auto& book : books)
        for (const auto& event : data.events) book.apply(event, data.bids(event), data.asks(event));
    const auto stop = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
    if (elapsed <= 0) throw std::runtime_error("Non-positive benchmark elapsed time");
    for (const auto& book : books) {
        if (book.state() != expected.current || book.last_valid_before_reset() != expected.last_valid_before_reset ||
            book.checksum() != expected.checksum) throw std::runtime_error("Post-run book/checksum mismatch");
    }
    CoreBenchmark out;
    out.engine = engine; out.repeats = repeats; out.source_events = data.events.size();
    out.source_market_events = expected.market_events; out.total_events = repeats * out.source_events;
    out.elapsed_ns = static_cast<std::uint64_t>(elapsed); out.checksum = expected.checksum;
    out.verified_history_checksum = expected.history_checksum;
    out.has_last_valid = expected.last_valid_before_reset.has_value();
    if (out.has_last_valid) out.last_valid_checksum = state_checksum(data.instrument, *expected.last_valid_before_reset);
    return out;
}
}
ReplayResult verify_engines(const EventData& data) {
    validate_events(data);
    MapBook map(data.instrument); VectorBook vector(data.instrument);
    ReplayResult result; result.instrument = data.instrument;
    Fnv1a64 history; history.bytes("BFGTRACE1");
    for (const auto& e : data.events) {
        map.apply(e, data.bids(e), data.asks(e)); vector.apply(e, data.bids(e), data.asks(e));
        if (map.state() != vector.state() || map.last_valid_before_reset() != vector.last_valid_before_reset() ||
            map.checksum() != vector.checksum() || map.best_bid() != vector.best_bid() ||
            map.best_ask() != vector.best_ask() || map.spread() != vector.spread())
            throw std::runtime_error("Map/vector mismatch at event " + std::to_string(result.events));
        ++result.events;
        if (e.kind == EventKind::reset) ++result.resets; else ++result.market_events;
        history.byte(static_cast<std::uint8_t>(e.kind)); history.integer(e.session);
        history.integer(e.update_id); history.integer(e.sequence); history.integer(map.checksum());
    }
    result.current = map.state(); result.last_valid_before_reset = map.last_valid_before_reset();
    result.checksum = map.checksum(); result.history_checksum = history.value();
    return result;
}
CoreBenchmark benchmark_core(const EventData& data, Engine engine, std::uint64_t repeats) {
    if (!repeats || repeats > 1000) throw std::runtime_error("--repeat must be in 1..1000");
    if (data.events.size() > std::numeric_limits<std::uint64_t>::max() / repeats)
        throw std::runtime_error("Benchmark event count overflow");
    // Full history verification is a separate untimed pass for each invocation.
    const auto expected = verify_engines(data);
    if (engine == Engine::map) return measure<MapBook>(data, engine, repeats, expected);
    return measure<VectorBook>(data, engine, repeats, expected);
}
std::string benchmark_json(const CoreBenchmark& r) {
    return "{\"schema\":1,\"mode\":\"core_only_preliminary\",\"engine\":\"" + std::string(engine_name(r.engine)) +
        "\",\"repeat\":" + std::to_string(r.repeats) + ",\"source_events\":" + std::to_string(r.source_events) +
        ",\"source_market_events\":" + std::to_string(r.source_market_events) +
        ",\"events\":" + std::to_string(r.total_events) + ",\"elapsed_ns\":" + std::to_string(r.elapsed_ns) +
        ",\"checksum\":\"" + hex_checksum(r.checksum) + "\",\"verified_history_checksum\":\"" +
        hex_checksum(r.verified_history_checksum) + "\",\"last_valid_checksum\":" +
        (r.has_last_valid ? '"' + hex_checksum(r.last_valid_checksum) + '"' : "null") +
        ",\"validation_in_timed_apply\":true,\"checksum_in_timed_loop\":false}";
}
}
