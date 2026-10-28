#include "order_book.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace feedguard {
namespace {
void require(bool okay, std::string_view why) {
    if (!okay) throw std::runtime_error(std::string(why));
}
template<class Map> void stage_new(Map& staged, const Map& current, std::span<const Level> changes) {
    for (const auto& level : changes)
        if (level.quantity && current.find(level.price) == current.end())
            staged.emplace(level.price, level.quantity);
}
template<class Map> void commit(Map& current, Map& staged, std::span<const Level> changes) {
    for (const auto& level : changes) {
        auto found = current.find(level.price);
        if (found == current.end()) continue;
        if (!level.quantity) current.erase(found);
        else found->second = level.quantity;
    }
    // Equal std::allocators; nodes are transferred without allocation.
    // Integer comparators do not throw, so no fallible operation remains here.
    current.merge(staged);
}
}
std::optional<Level> BookState::best_bid() const {
    return valid && !bids.empty() ? std::optional<Level>{bids.front()} : std::nullopt;
}
std::optional<Level> BookState::best_ask() const {
    return valid && !asks.empty() ? std::optional<Level>{asks.front()} : std::nullopt;
}
std::optional<std::int64_t> BookState::spread() const {
    const auto bid = best_bid(), ask = best_ask();
    return bid && ask ? std::optional<std::int64_t>{ask->price - bid->price} : std::nullopt;
}
Fnv1a64 begin_book_checksum(const Instrument& m, bool valid, std::uint64_t session,
    std::uint64_t u, std::uint64_t seq, std::uint64_t bids, std::uint64_t asks) noexcept {
    Fnv1a64 hash;
    hash.bytes("BFGBOOK1");
    hash.integer(m.price_decimals, 4); hash.integer(m.quantity_decimals, 4);
    hash.integer(static_cast<std::uint64_t>(m.tick_size));
    hash.integer(static_cast<std::uint64_t>(m.quantity_step));
    hash.byte(valid ? 1 : 0); hash.integer(session); hash.integer(u); hash.integer(seq);
    hash.integer(bids); hash.integer(asks);
    return hash;
}
std::uint64_t state_checksum(const Instrument& m, const BookState& state) noexcept {
    auto hash = begin_book_checksum(m, state.valid, state.session, state.update_id, state.sequence,
                                   state.bids.size(), state.asks.size());
    for (auto side : {std::span<const Level>(state.bids), std::span<const Level>(state.asks)})
        for (auto level : side) {
            hash.integer(static_cast<std::uint64_t>(level.price));
            hash.integer(static_cast<std::uint64_t>(level.quantity));
        }
    return hash.value();
}
std::string hex_checksum(std::uint64_t checksum) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (unsigned i = 0; i < 16; ++i) text[15 - i] = digits[(checksum >> (4 * i)) & 15];
    return text;
}
MapBook::MapBook(Instrument instrument) : instrument_(std::move(instrument)) {
    require(instrument_.price_decimals <= 18 && instrument_.quantity_decimals <= 18 &&
        instrument_.tick_size > 0 && instrument_.quantity_step > 0, "Invalid book instrument");
    scratch_prices_.reserve(128);
}
void MapBook::apply(const MarketEvent& e, std::span<const Level> bids, std::span<const Level> asks) {
    validate_book_event(instrument_, valid_, session_, last_steady_, e, bids, asks, scratch_prices_); // No externally visible state changes until all checks succeed.
    if (e.kind == EventKind::reset) {
        if (valid_) last_valid_ = state(); // Allocation, if needed, happens before invalidation.
        bids_.clear(); asks_.clear(); valid_ = false;
        session_ = e.session; update_id_ = 0; sequence_ = 0;
        return;
    }
    Bids staged_bids;
    Asks staged_asks;
    if (e.kind == EventKind::snapshot) {
        for (auto level : bids) staged_bids.emplace_hint(staged_bids.end(), level.price, level.quantity);
        for (auto level : asks) staged_asks.emplace_hint(staged_asks.end(), level.price, level.quantity);
        bids_.swap(staged_bids); asks_.swap(staged_asks);
    } else {
        stage_new(staged_bids, bids_, bids);
        stage_new(staged_asks, asks_, asks);
        commit(bids_, staged_bids, bids);
        commit(asks_, staged_asks, asks);
    }
    valid_ = true; session_ = e.session; update_id_ = e.update_id; sequence_ = e.sequence;
    last_steady_ = e.receive_steady_ns;
}
BookState MapBook::state() const {
    BookState out;
    out.valid = valid_; out.session = session_; out.update_id = update_id_; out.sequence = sequence_;
    out.bids.reserve(bids_.size()); out.asks.reserve(asks_.size());
    for (auto [price, quantity] : bids_) out.bids.push_back({price, quantity});
    for (auto [price, quantity] : asks_) out.asks.push_back({price, quantity});
    return out;
}
std::optional<Level> MapBook::best_bid() const {
    return valid_ && !bids_.empty() ? std::optional<Level>{{bids_.begin()->first, bids_.begin()->second}} : std::nullopt;
}
std::optional<Level> MapBook::best_ask() const {
    return valid_ && !asks_.empty() ? std::optional<Level>{{asks_.begin()->first, asks_.begin()->second}} : std::nullopt;
}
std::optional<std::int64_t> MapBook::spread() const {
    const auto bid = best_bid(), ask = best_ask();
    return bid && ask ? std::optional<std::int64_t>{ask->price - bid->price} : std::nullopt;
}
std::uint64_t MapBook::checksum() const noexcept {
    auto hash = begin_book_checksum(instrument_, valid_, session_, update_id_, sequence_, bids_.size(), asks_.size());
    for (const auto& [price, quantity] : bids_) {
        hash.integer(static_cast<std::uint64_t>(price)); hash.integer(static_cast<std::uint64_t>(quantity));
    }
    for (const auto& [price, quantity] : asks_) {
        hash.integer(static_cast<std::uint64_t>(price)); hash.integer(static_cast<std::uint64_t>(quantity));
    }
    return hash.value();
}
ReplayResult replay_map(const EventData& data) {
    validate_events(data);
    MapBook book(data.instrument);
    Fnv1a64 history;
    history.bytes("BFGTRACE1");
    ReplayResult result;
    result.instrument = data.instrument;
    for (const auto& event : data.events) {
        book.apply(event, data.bids(event), data.asks(event));
        ++result.events;
        if (event.kind == EventKind::reset) ++result.resets; else ++result.market_events;
        history.byte(static_cast<std::uint8_t>(event.kind));
        history.integer(event.session); history.integer(event.update_id); history.integer(event.sequence);
        history.integer(book.checksum());
    }
    result.current = book.state(); result.last_valid_before_reset = book.last_valid_before_reset();
    result.checksum = book.checksum(); result.history_checksum = history.value();
    return result;
}
}
