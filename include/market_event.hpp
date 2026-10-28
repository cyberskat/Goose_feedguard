#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace feedguard {
struct Instrument {
    std::uint32_t price_decimals{}, quantity_decimals{};
    std::int64_t tick_size{}, quantity_step{};
    std::array<std::uint8_t, 32> metadata_sha256{};
    bool operator==(const Instrument&) const = default;
};
struct Level {
    std::int64_t price{}, quantity{};
    bool operator==(const Level&) const = default;
};
enum class EventKind : std::uint8_t { snapshot = 1, delta = 2, reset = 3 };
struct MarketEvent {
    EventKind kind{};
    std::uint64_t session{}, update_id{}, sequence{}, exchange_ms{}, matching_ms{};
    std::int64_t receive_utc_ns{}, receive_steady_ns{};
    std::uint64_t first_level{};
    std::uint32_t bid_count{}, ask_count{};
    bool operator==(const MarketEvent&) const = default;
};
struct EventData {
    Instrument instrument;
    std::vector<MarketEvent> events;
    std::vector<Level> levels;
    bool operator==(const EventData&) const = default;
    std::span<const Level> bids(const MarketEvent& e) const {
        return std::span<const Level>(levels).subspan(static_cast<std::size_t>(e.first_level), e.bid_count);
    }
    std::span<const Level> asks(const MarketEvent& e) const {
        return std::span<const Level>(levels).subspan(static_cast<std::size_t>(e.first_level) + e.bid_count, e.ask_count);
    }
};
std::int64_t decimal_to_int64(std::string_view text, std::uint32_t decimals);
Instrument read_instrument(const std::filesystem::path& path);
EventData convert_capture(const std::filesystem::path& path, const Instrument& instrument);
void validate_events(const EventData& data);
std::vector<std::uint8_t> encode_events(const EventData& data);
EventData decode_events(std::span<const std::uint8_t> bytes);
void write_events(const std::filesystem::path& path, const EventData& data);
EventData read_events(const std::filesystem::path& path);
std::uint32_t crc32(std::span<const std::uint8_t> bytes);
}
