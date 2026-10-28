#include "order_book.hpp"
#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace feedguard {
namespace {
static_assert(std::is_nothrow_copy_constructible_v<Level> && std::is_nothrow_move_assignable_v<Level>);
template<bool Bid> auto position(std::vector<Level>& side, std::int64_t price) {
    return std::lower_bound(side.begin(), side.end(), price, [](Level l, std::int64_t p) {
        if constexpr (Bid) return l.price > p; else return l.price < p;
    });
}
template<bool Bid> std::size_t required_size(std::vector<Level>& side, std::span<const Level> changes) {
    auto size = side.size();
    for (auto l : changes) {
        const auto found = position<Bid>(side, l.price);
        const bool exists = found != side.end() && found->price == l.price;
        if (l.quantity && !exists) {
            if (size == side.max_size()) throw std::length_error("Vector book capacity overflow");
            ++size;
        } else if (!l.quantity && exists) --size;
    }
    return size;
}
void reserve_growth(std::vector<Level>& side, std::size_t needed) {
    if (needed <= side.capacity()) return;
    const auto grown = side.capacity() <= side.max_size() / 2 ? side.capacity() * 2 : side.max_size();
    side.reserve(std::max(needed, grown));
}
template<bool Bid> void commit(std::vector<Level>& side, std::span<const Level> changes) {
    // Delete first: peak size never exceeds max(old size, validated final size).
    for (auto l : changes) if (!l.quantity) {
        auto found = position<Bid>(side, l.price);
        if (found != side.end() && found->price == l.price) side.erase(found);
    }
    for (auto l : changes) if (l.quantity) {
        auto found = position<Bid>(side, l.price);
        if (found != side.end() && found->price == l.price) found->quantity = l.quantity;
        else side.insert(found, l); // capacity is already sufficient; Level copy/move cannot throw
    }
}
}
VectorBook::VectorBook(Instrument instrument) : instrument_(std::move(instrument)) {
    if (instrument_.price_decimals > 18 || instrument_.quantity_decimals > 18 ||
        instrument_.tick_size <= 0 || instrument_.quantity_step <= 0)
        throw std::runtime_error("Invalid book instrument");
    bids_.reserve(64); asks_.reserve(64); scratch_prices_.reserve(128);
}
void VectorBook::apply(const MarketEvent& e, std::span<const Level> bids, std::span<const Level> asks) {
    validate_book_event(instrument_, valid_, session_, last_steady_, e, bids, asks, scratch_prices_);
    if (e.kind == EventKind::reset) {
        if (valid_) last_valid_ = state();
        bids_.clear(); asks_.clear(); valid_ = false;
        session_ = e.session; update_id_ = 0; sequence_ = 0;
        return;
    }
    if (e.kind == EventKind::snapshot) {
        reserve_growth(bids_, bids.size()); reserve_growth(asks_, asks.size());
        bids_.assign(bids.begin(), bids.end()); asks_.assign(asks.begin(), asks.end());
    } else {
        const auto bid_size = required_size<true>(bids_, bids);
        const auto ask_size = required_size<false>(asks_, asks);
        // Both potentially throwing allocations precede the first content change.
        reserve_growth(bids_, bid_size); reserve_growth(asks_, ask_size);
        commit<true>(bids_, bids); commit<false>(asks_, asks);
    }
    valid_ = true; session_ = e.session; update_id_ = e.update_id; sequence_ = e.sequence;
    last_steady_ = e.receive_steady_ns;
}
BookState VectorBook::state() const { return {valid_, session_, update_id_, sequence_, bids_, asks_}; }
std::optional<Level> VectorBook::best_bid() const {
    return valid_ && !bids_.empty() ? std::optional<Level>{bids_.front()} : std::nullopt;
}
std::optional<Level> VectorBook::best_ask() const {
    return valid_ && !asks_.empty() ? std::optional<Level>{asks_.front()} : std::nullopt;
}
std::optional<std::int64_t> VectorBook::spread() const {
    const auto bid = best_bid(), ask = best_ask();
    return bid && ask ? std::optional<std::int64_t>{ask->price - bid->price} : std::nullopt;
}
std::uint64_t VectorBook::checksum() const noexcept {
    auto hash = begin_book_checksum(instrument_, valid_, session_, update_id_, sequence_, bids_.size(), asks_.size());
    for (const auto& side : {std::span<const Level>(bids_), std::span<const Level>(asks_)})
        for (auto l : side) { hash.integer(static_cast<std::uint64_t>(l.price)); hash.integer(static_cast<std::uint64_t>(l.quantity)); }
    return hash.value();
}
ReplayResult replay_vector(const EventData& data) {
    validate_events(data);
    VectorBook book(data.instrument);
    Fnv1a64 history; history.bytes("BFGTRACE1");
    ReplayResult result; result.instrument = data.instrument;
    for (const auto& e : data.events) {
        book.apply(e, data.bids(e), data.asks(e));
        ++result.events;
        if (e.kind == EventKind::reset) ++result.resets; else ++result.market_events;
        history.byte(static_cast<std::uint8_t>(e.kind)); history.integer(e.session);
        history.integer(e.update_id); history.integer(e.sequence); history.integer(book.checksum());
    }
    result.current = book.state(); result.last_valid_before_reset = book.last_valid_before_reset();
    result.checksum = book.checksum(); result.history_checksum = history.value();
    return result;
}
std::string_view engine_name(Engine engine) noexcept { return engine == Engine::map ? "cpp_map" : "cpp_vector"; }
}
