#include "order_book.hpp"
#include <algorithm>
#include <stdexcept>
namespace feedguard {
namespace {
void require(bool okay, std::string_view why) { if (!okay) throw std::runtime_error(std::string(why)); }
}
void validate_book_event(const Instrument& instrument, bool valid, std::uint64_t session,
    std::int64_t last_steady, const MarketEvent& e, std::span<const Level> bids,
    std::span<const Level> asks, std::vector<std::int64_t>& scratch) {
    require(e.session > 0 && e.receive_utc_ns > 0 && e.receive_steady_ns >= 0, "Invalid event session/timestamp");
    require(e.bid_count == bids.size() && e.ask_count == asks.size(), "Event level count mismatch");
    require(bids.size() <= 4096 && asks.size() <= 4096, "Too many level changes");
    if (e.kind == EventKind::reset) {
        require(e.session >= session && bids.empty() && asks.empty() && e.update_id == 0 &&
            e.sequence == 0 && e.exchange_ms == 0 && e.matching_ms == 0 && e.receive_steady_ns == 0,
            "Invalid reset event");
        return;
    }
    require(e.kind == EventKind::snapshot || e.kind == EventKind::delta, "Unknown event kind");
    require(e.update_id > 0 && e.exchange_ms > 0 && e.receive_steady_ns >= last_steady, "Invalid market metadata");
    require(session == 0 || e.session == session, "New session requires reset");
    if (e.kind == EventKind::delta) require(valid && e.update_id != 1, "Delta requires current snapshot");
    else require(bids.size() <= 50 && asks.size() <= 50, "Snapshot exceeds subscribed depth");
    for (unsigned index = 0; index < 2; ++index) {
        const auto side = index == 0 ? bids : asks;
        scratch.clear();
        for (std::size_t i = 0; i < side.size(); ++i) {
            const auto level = side[i];
            require(level.price > 0 && level.quantity >= 0 &&
                (e.kind != EventKind::snapshot || level.quantity > 0), "Invalid level price/quantity");
            require(level.price % instrument.tick_size == 0 && level.quantity % instrument.quantity_step == 0,
                "Level off instrument grid");
            if (e.kind == EventKind::snapshot && i)
                require(index == 0 ? side[i - 1].price > level.price : side[i - 1].price < level.price,
                    "Unsorted snapshot");
            scratch.push_back(level.price);
        }
        std::sort(scratch.begin(), scratch.end());
        require(std::adjacent_find(scratch.begin(), scratch.end()) == scratch.end(),
            "Duplicate level price");
    }
}
}
