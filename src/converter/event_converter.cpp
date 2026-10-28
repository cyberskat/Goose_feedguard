#include "market_event.hpp"
#include <simdjson.h>
#include <openssl/evp.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace feedguard {
namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
template<class T> T field(simdjson::dom::element doc, std::string_view name) {
    T out{};
    if (auto error = doc[name].get(out))
        throw std::runtime_error("Invalid/missing field " + std::string(name) + ": " + simdjson::error_message(error));
    return out;
}
simdjson::dom::element element(simdjson::dom::element doc, std::string_view name) {
    simdjson::dom::element out;
    if (auto error = doc[name].get(out))
        throw std::runtime_error("Invalid/missing object " + std::string(name) + ": " + simdjson::error_message(error));
    return out;
}
std::string load_metadata(const std::filesystem::path& path) {
    const auto n = std::filesystem::file_size(path);
    require(n > 0 && n <= 1024 * 1024, "Metadata must be 1 byte to 1 MiB");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot open metadata");
    std::string bytes(static_cast<std::size_t>(n), '\0');
    require(static_cast<bool>(input.read(bytes.data(), static_cast<std::streamsize>(n))), "Cannot read metadata");
    return bytes;
}
std::pair<std::uint32_t, std::int64_t> step(std::string_view value) {
    const auto original = value;
    // Remove insignificant trailing fractional zeros; retain the exact step integer.
    const auto dot = value.find('.');
    if (dot != std::string_view::npos) {
        while (!value.empty() && value.back() == '0') value.remove_suffix(1);
        if (!value.empty() && value.back() == '.') value.remove_suffix(1);
    }
    const auto position = value.find('.');
    const auto scale = position == std::string_view::npos ? 0u : static_cast<std::uint32_t>(value.size() - position - 1);
    require(scale <= 18, "Instrument precision exceeds 18 decimals");
    const auto ticks = decimal_to_int64(original, scale);
    require(ticks > 0, "Instrument step must be positive");
    return {scale, ticks};
}
void append_levels(EventData& out, simdjson::dom::element data, std::string_view name,
                   bool snapshot, std::uint32_t& count) {
    auto levels = field<simdjson::dom::array>(data, name);
    require(levels.size() <= 4096, "Too many changes on one side");
    count = static_cast<std::uint32_t>(levels.size());
    for (auto entry : levels) {
        simdjson::dom::array pair;
        require(!entry.get(pair) && pair.size() == 2, "Level must have exactly price and quantity");
        std::string_view price, quantity;
        require(!pair.at(0).get(price) && !pair.at(1).get(quantity), "Level fields must be decimal strings");
        require(!price.empty() && !quantity.empty() && price.front() != '-' && quantity.front() != '-', "Negative market value");
        const auto p = decimal_to_int64(price, out.instrument.price_decimals);
        const auto q = decimal_to_int64(quantity, out.instrument.quantity_decimals);
        require(p > 0 && q >= 0 && (!snapshot || q > 0), "Invalid price/quantity");
        require(p % out.instrument.tick_size == 0 && q % out.instrument.quantity_step == 0, "Price/quantity off instrument grid");
        out.levels.push_back({p, q});
    }
}
}

std::int64_t decimal_to_int64(std::string_view text, std::uint32_t decimals) {
    require(decimals <= 18 && !text.empty(), "Invalid decimal scale or empty decimal");
    bool negative = text.front() == '-';
    if (negative) text.remove_prefix(1);
    require(!text.empty() && text.front() >= '0' && text.front() <= '9', "Invalid decimal syntax");
    std::uint64_t magnitude = 0;
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1u : 0u);
    bool dot = false;
    std::uint32_t fraction = 0;
    auto digit = [&](unsigned value) {
        require(magnitude <= (limit - value) / 10, "Decimal overflows int64");
        magnitude = magnitude * 10 + value;
    };
    for (const char c : text) {
        if (c == '.' && !dot) { dot = true; continue; }
        require(c >= '0' && c <= '9', "Invalid decimal syntax");
        if (!dot) digit(static_cast<unsigned>(c - '0'));
        else {
            if (fraction < decimals) digit(static_cast<unsigned>(c - '0'));
            else require(c == '0', "Decimal precision would be lost");
            // Saturate the counter: long trailing zeros cannot overflow it.
            if (fraction <= decimals) ++fraction;
        }
    }
    require(text.back() != '.', "Decimal cannot end with a dot");
    while (fraction < decimals) { digit(0); ++fraction; }
    if (!negative) return static_cast<std::int64_t>(magnitude);
    if (magnitude == (std::uint64_t{1} << 63)) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(magnitude);
}

Instrument read_instrument(const std::filesystem::path& path) {
    auto bytes = load_metadata(path);
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    require(!parser.parse(bytes).get(doc), "Malformed instrument JSON");
    require(field<std::uint64_t>(doc, "retCode") == 0, "Bybit returned metadata error");
    const auto result = element(doc, "result");
    require(field<std::string_view>(result, "category") == "linear", "Metadata category is not linear");
    auto list = field<simdjson::dom::array>(result, "list");
    require(list.size() == 1, "Metadata must identify exactly one BTCUSDT instrument");
    auto item = list.at(0).value();
    require(field<std::string_view>(item, "symbol") == "BTCUSDT", "Metadata symbol is not BTCUSDT");
    auto [price_decimals, tick_size] = step(field<std::string_view>(element(item, "priceFilter"), "tickSize"));
    auto [quantity_decimals, quantity_step] = step(field<std::string_view>(element(item, "lotSizeFilter"), "qtyStep"));
    Instrument out{price_decimals, quantity_decimals, tick_size, quantity_step, {}};
    unsigned length = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), out.metadata_sha256.data(), &length, EVP_sha256(), nullptr) == 1 &&
        length == out.metadata_sha256.size(), "Metadata SHA-256 failed");
    return out;
}

EventData convert_capture(const std::filesystem::path& path, const Instrument& instrument) {
    require(instrument.tick_size > 0 && instrument.quantity_step > 0, "Invalid instrument steps");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot open capture");
    EventData out;
    out.instrument = instrument;
    const auto estimate = std::min<std::uint64_t>(std::filesystem::file_size(path) / 300, 1000000);
    out.events.reserve(static_cast<std::size_t>(estimate));
    out.levels.reserve(static_cast<std::size_t>(estimate) * 8);
    simdjson::dom::parser parser;
    // Collector permits 1 MiB websocket payloads; allow room for the JSONL envelope.
    constexpr std::size_t max_line = 2 * 1024 * 1024;
    std::vector<char> buffer(max_line + 2);
    std::uint64_t active = 0, last_session = 0;
    bool ready = false, stopped = false;
    std::size_t line_number = 0;
    while (true) {
        input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto length = input.gcount();
        if (!length && input.eof()) break;
        ++line_number;
        try {
            require(!input.bad() && (!input.fail() || input.eof()), "Capture line exceeds 2 MiB or read failed");
            if (!input.eof()) --length; // getline counts the consumed delimiter
            require(length > 0 && length <= static_cast<std::streamsize>(max_line), "Empty or oversized capture line");
            require(!stopped, "Records after collector_stop");
            simdjson::dom::element doc;
            require(!parser.parse(buffer.data(), static_cast<std::size_t>(length)).get(doc), "Malformed capture JSON");
            require(field<std::uint64_t>(doc, "schema") == 1, "Unsupported capture schema");
            const auto kind = field<std::string_view>(doc, "kind");
            const auto session = field<std::uint64_t>(doc, "session");
            const auto utc = field<std::int64_t>(doc, "recv_utc_ns");
            require(utc > 0 && session > 0, "Invalid timestamp or session");
            if (kind == "session_start" || kind == "disconnect") {
                if (kind == "session_start") {
                    require(active == 0 && session > last_session, "Invalid session_start");
                    active = session; last_session = session;
                } else {
                    require(active == session, "Disconnect outside active session");
                    active = 0;
                }
                ready = false;
                MarketEvent reset;
                reset.kind = EventKind::reset; reset.session = session; reset.receive_utc_ns = utc;
                reset.first_level = out.levels.size(); out.events.push_back(reset);
            } else if (kind == "snapshot" || kind == "delta") {
                require(active == session && field<bool>(doc, "accepted"), "Market event outside active session");
                const auto raw = element(doc, "raw");
                require(field<std::string_view>(raw, "topic") == "orderbook.50.BTCUSDT" &&
                    field<std::string_view>(raw, "type") == kind, "Unexpected market topic/type");
                auto data = element(raw, "data");
                require(field<std::string_view>(data, "s") == "BTCUSDT", "Unexpected market symbol");
                const bool snapshot = kind == "snapshot";
                require(snapshot || ready, "Delta before snapshot");
                MarketEvent e;
                e.kind = snapshot ? EventKind::snapshot : EventKind::delta;
                e.session = session; e.receive_utc_ns = utc;
                e.receive_steady_ns = field<std::int64_t>(doc, "recv_steady_ns");
                e.update_id = field<std::uint64_t>(data, "u"); e.sequence = field<std::uint64_t>(data, "seq");
                e.exchange_ms = field<std::uint64_t>(raw, "ts");
                auto cts_error = raw["cts"].get(e.matching_ms);
                require(!cts_error || cts_error == simdjson::NO_SUCH_FIELD, "Invalid cts");
                e.first_level = out.levels.size();
                append_levels(out, data, "b", snapshot, e.bid_count);
                append_levels(out, data, "a", snapshot, e.ask_count);
                out.events.push_back(e);
                ready = true;
            } else if (kind == "control") {
                require(active == session && !field<bool>(doc, "accepted"), "Invalid control envelope");
                auto raw = element(doc, "raw");
                simdjson::dom::object object;
                require(!raw.get(object), "Control payload must be an object");
                // A rejected subscription cannot be presented as a successful capture.
                std::string_view op;
                if (!raw["op"].get(op) && op == "subscribe") require(field<bool>(raw, "success"), "Subscription rejected");
            } else if (kind == "collector_stop") {
                require(active == 0 && session == last_session, "Invalid collector_stop");
                stopped = true;
            } else throw std::runtime_error("Unknown/invalid journal record kind");
        } catch (const std::exception& error) {
            throw std::runtime_error("Capture line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    require(stopped && active == 0, "Incomplete capture: collector_stop or disconnect missing");
    require(std::any_of(out.events.begin(), out.events.end(), [](const auto& e) { return e.kind == EventKind::snapshot; }), "Capture contains no snapshot");
    validate_events(out);
    return out;
}
}
