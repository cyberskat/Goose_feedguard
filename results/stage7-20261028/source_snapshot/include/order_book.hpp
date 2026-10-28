#pragma once
#include "market_event.hpp"
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace feedguard {
struct BookState {
    bool valid = false;
    std::uint64_t session{}, update_id{}, sequence{};
    std::vector<Level> bids, asks;
    bool operator==(const BookState&) const = default;
    std::optional<Level> best_bid() const;
    std::optional<Level> best_ask() const;
    std::optional<std::int64_t> spread() const;
};

// Unsigned overflow is defined modulo 2^64; no signed arithmetic in the hash.
class Fnv1a64 {
    std::uint64_t value_ = 14695981039346656037ull;
public:
    void byte(std::uint8_t value) noexcept { value_ = (value_ ^ value) * 1099511628211ull; }
    void bytes(std::string_view text) noexcept { for (unsigned char c : text) byte(c); }
    void integer(std::uint64_t value, unsigned width = 8) noexcept {
        for (unsigned i = 0; i < width; ++i) byte(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    std::uint64_t value() const noexcept { return value_; }
};
Fnv1a64 begin_book_checksum(const Instrument& instrument, bool valid, std::uint64_t session,
    std::uint64_t u, std::uint64_t seq, std::uint64_t bids, std::uint64_t asks) noexcept;
std::uint64_t state_checksum(const Instrument& instrument, const BookState& state) noexcept;
std::string hex_checksum(std::uint64_t checksum);
enum class Engine { map, vector };
std::string_view engine_name(Engine engine) noexcept;
void validate_book_event(const Instrument& instrument, bool valid, std::uint64_t session,
    std::int64_t last_steady, const MarketEvent& event, std::span<const Level> bids,
    std::span<const Level> asks, std::vector<std::int64_t>& scratch);

class MapBook {
    using Bids = std::map<std::int64_t, std::int64_t, std::greater<std::int64_t>>;
    using Asks = std::map<std::int64_t, std::int64_t>;
    Instrument instrument_;
    Bids bids_;
    Asks asks_;
    std::vector<std::int64_t> scratch_prices_;
    std::optional<BookState> last_valid_;
    bool valid_ = false;
    std::uint64_t session_ = 0, update_id_ = 0, sequence_ = 0;
    std::int64_t last_steady_ = 0;
public:
    explicit MapBook(Instrument instrument);
    void apply(const MarketEvent& event, std::span<const Level> bids, std::span<const Level> asks);
    BookState state() const;
    const std::optional<BookState>& last_valid_before_reset() const noexcept { return last_valid_; }
    bool valid() const noexcept { return valid_; }
    std::optional<Level> best_bid() const;
    std::optional<Level> best_ask() const;
    std::optional<std::int64_t> spread() const;
    std::uint64_t checksum() const noexcept;
};

class VectorBook {
    Instrument instrument_;
    std::vector<Level> bids_, asks_;
    std::vector<std::int64_t> scratch_prices_;
    std::optional<BookState> last_valid_;
    bool valid_ = false;
    std::uint64_t session_ = 0, update_id_ = 0, sequence_ = 0;
    std::int64_t last_steady_ = 0;
public:
    explicit VectorBook(Instrument instrument);
    void apply(const MarketEvent& event, std::span<const Level> bids, std::span<const Level> asks);
    BookState state() const;
    const std::optional<BookState>& last_valid_before_reset() const noexcept { return last_valid_; }
    bool valid() const noexcept { return valid_; }
    std::optional<Level> best_bid() const;
    std::optional<Level> best_ask() const;
    std::optional<std::int64_t> spread() const;
    std::uint64_t checksum() const noexcept;
    std::size_t bid_capacity() const noexcept { return bids_.capacity(); }
    std::size_t ask_capacity() const noexcept { return asks_.capacity(); }
};

struct ReplayResult {
    Instrument instrument;
    std::uint64_t events = 0, market_events = 0, resets = 0;
    std::uint64_t checksum = 0, history_checksum = 0;
    BookState current;
    std::optional<BookState> last_valid_before_reset;
    bool operator==(const ReplayResult&) const = default;
};
ReplayResult replay_map(const EventData& data);
ReplayResult replay_vector(const EventData& data);
ReplayResult verify_engines(const EventData& data);
std::string replay_json(const ReplayResult& result, Engine engine = Engine::map);
struct CoreBenchmark {
    Engine engine{};
    std::uint64_t repeats{}, source_events{}, source_market_events{}, total_events{}, elapsed_ns{};
    std::uint64_t checksum{}, last_valid_checksum{}, verified_history_checksum{};
    bool has_last_valid = false;
};
CoreBenchmark benchmark_core(const EventData& data, Engine engine, std::uint64_t repeats);
std::string benchmark_json(const CoreBenchmark& result);
std::string measure_run(const std::filesystem::path& input, Engine engine,
                        std::string_view mode, bool latencies);
}
