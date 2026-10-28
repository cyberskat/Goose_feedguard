#include "order_book.hpp"
#include <charconv>
#include <stdexcept>

namespace feedguard {
namespace {
template<class Number> void number(std::string& out, Number n) {
    char buffer[32];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), n);
    if (error != std::errc{}) throw std::runtime_error("Cannot serialize replay integer");
    out.append(buffer, end);
}
void level(std::string& out, std::optional<Level> value) {
    if (!value) { out += "null"; return; }
    out += '['; number(out, value->price); out += ','; number(out, value->quantity); out += ']';
}
void state(std::string& out, const BookState& s, const Instrument& instrument) {
    out += "{\"valid\":"; out += s.valid ? "true" : "false";
    out += ",\"session\":"; number(out, s.session);
    out += ",\"u\":"; number(out, s.update_id);
    out += ",\"seq\":"; number(out, s.sequence);
    out += ",\"checksum\":\"" + hex_checksum(state_checksum(instrument, s)) + '"';
    out += ",\"best_bid\":"; level(out, s.best_bid());
    out += ",\"best_ask\":"; level(out, s.best_ask());
    out += ",\"spread\":";
    if (const auto spread = s.spread()) number(out, *spread); else out += "null";
    out += ",\"bids\":[";
    bool first = true;
    for (auto item : s.bids) { if (!first) out += ','; first = false; level(out, item); }
    out += "],\"asks\":["; first = true;
    for (auto item : s.asks) { if (!first) out += ','; first = false; level(out, item); }
    out += "]}";
}
}
std::string replay_json(const ReplayResult& r, Engine engine) {
    std::string out;
    out.reserve(8192);
    out += "{\"schema\":1,\"engine\":\"";
    out += engine_name(engine);
    out += "\",\"mode\":\"verify_replay\",\"events\":"; number(out, r.events);
    out += ",\"market_events\":"; number(out, r.market_events);
    out += ",\"resets\":"; number(out, r.resets);
    out += ",\"price_decimals\":"; number(out, r.instrument.price_decimals);
    out += ",\"quantity_decimals\":"; number(out, r.instrument.quantity_decimals);
    out += ",\"checksum\":\"" + hex_checksum(r.checksum) + '"';
    out += ",\"history_checksum\":\"" + hex_checksum(r.history_checksum) + '"';
    out += ",\"current\":"; state(out, r.current, r.instrument);
    out += ",\"last_valid_before_reset\":";
    if (r.last_valid_before_reset) state(out, *r.last_valid_before_reset, r.instrument); else out += "null";
    out += '}';
    return out;
}
}
