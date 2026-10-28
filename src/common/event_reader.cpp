#include "market_event.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cstdio>

namespace feedguard {
namespace {
constexpr std::size_t header_size = 128, record_size = 72;
constexpr std::uint64_t file_limit = 1024ull * 1024 * 1024;
constexpr std::uint32_t level_limit = 4096;
constexpr std::array<std::uint8_t, 8> magic{'B','F','G','D','E','V','T','1'};
void require(bool okay, std::string_view why) {
    if (!okay) throw std::runtime_error(std::string(why));
}
void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint64_t get(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned n) {
    require(offset <= bytes.size() && n <= bytes.size() - offset, "Truncated binary integer");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}
std::int64_t signed_get(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::bit_cast<std::int64_t>(get(bytes, offset, 8));
}
void write_exclusive(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    int fd = -1;
#ifdef _WIN32
    const auto ec = _wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                              _SH_DENYWR, _S_IREAD | _S_IWRITE);
    require(!ec && fd >= 0, "Cannot create events file; existing files are never overwritten");
    auto* file = _fdopen(fd, "wb");
    if (!file) _close(fd);
#else
    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    require(fd >= 0, "Cannot create events file; existing files are never overwritten");
    auto* file = fdopen(fd, "wb");
    if (!file) ::close(fd);
#endif
    require(file != nullptr, "Cannot open events stream");
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    require(written && closed, "Failed writing/flushing events file (partial file may remain)");
}
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto x = i;
            for (unsigned k = 0; k < 8; ++k) x = (x >> 1) ^ ((x & 1) ? 0xedb88320u : 0u);
            t[i] = x;
        }
        return t;
    }();
    std::uint32_t value = 0xffffffffu;
    for (auto byte : bytes) value = table[(value ^ byte) & 255] ^ (value >> 8);
    return value ^ 0xffffffffu;
}

void validate_events(const EventData& data) {
    const auto& m = data.instrument;
    require(m.price_decimals <= 18 && m.quantity_decimals <= 18, "Invalid decimal scales");
    require(m.tick_size > 0 && m.quantity_step > 0, "Invalid instrument steps");
    bool ready = false;
    std::uint64_t session = 0, next_level = 0, last_steady = 0;
    std::vector<std::int64_t> prices;
    prices.reserve(256);
    for (const auto& e : data.events) {
        require(e.session > 0 && e.first_level == next_level, "Invalid session or non-contiguous level offset");
        require(e.receive_utc_ns > 0 && e.receive_steady_ns >= 0, "Invalid receive timestamp");
        require(e.bid_count <= level_limit && e.ask_count <= level_limit, "Too many level updates");
        auto count = static_cast<std::uint64_t>(e.bid_count) + e.ask_count;
        require(next_level <= data.levels.size() && count <= data.levels.size() - next_level, "Level range out of bounds");
        next_level += count;
        if (e.kind == EventKind::reset) {
            require(e.session >= session && count == 0 && e.update_id == 0 && e.sequence == 0 &&
                e.exchange_ms == 0 && e.matching_ms == 0 && e.receive_steady_ns == 0, "Invalid reset event");
            session = e.session; ready = false;
            continue;
        }
        require(e.kind == EventKind::snapshot || e.kind == EventKind::delta, "Unknown event kind");
        require(e.session == session && e.update_id > 0 && e.exchange_ms > 0, "Market event outside session");
        require(static_cast<std::uint64_t>(e.receive_steady_ns) >= last_steady, "Non-monotonic receive clock");
        last_steady = static_cast<std::uint64_t>(e.receive_steady_ns);
        if (e.kind == EventKind::snapshot) {
            require(e.bid_count <= 50 && e.ask_count <= 50, "Snapshot exceeds subscribed depth");
            ready = true;
        } else require(ready && e.update_id != 1, "Delta before snapshot or unexpected restart");
        for (unsigned side_index = 0; side_index < 2; ++side_index) {
            const auto side = side_index == 0 ? data.bids(e) : data.asks(e);
            prices.clear();
            std::int64_t previous = 0;
            for (auto level : side) {
                require(level.price > 0 && level.quantity >= 0, "Negative quantity or non-positive price");
                require(level.price % m.tick_size == 0 && level.quantity % m.quantity_step == 0, "Off-grid level");
                prices.push_back(level.price);
                if (e.kind == EventKind::snapshot) {
                    require(level.quantity > 0, "Zero quantity in snapshot");
                    if (previous) {
                        require(side_index == 0 ? previous > level.price : previous < level.price, "Unsorted snapshot");
                    }
                    previous = level.price;
                }
            }
            std::sort(prices.begin(), prices.end());
            require(std::adjacent_find(prices.begin(), prices.end()) == prices.end(), "Duplicate price on one side");
        }
    }
    require(next_level == data.levels.size(), "Unreferenced levels");
    require(!data.events.empty(), "Empty event stream");
}

std::vector<std::uint8_t> encode_events(const EventData& data) {
    validate_events(data);
    require(data.events.size() <= (file_limit - header_size) / record_size, "Too many events");
    auto length = header_size + data.events.size() * record_size;
    require(data.levels.size() <= (file_limit - length) / 16, "Events exceed 1 GiB limit");
    length += data.levels.size() * 16;
    std::vector<std::uint8_t> bytes(length, 0);
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put(bytes, 8, 1, 4); put(bytes, 12, header_size, 4);
    put(bytes, 16, data.instrument.price_decimals, 4); put(bytes, 20, data.instrument.quantity_decimals, 4);
    put(bytes, 24, static_cast<std::uint64_t>(data.instrument.tick_size), 8);
    put(bytes, 32, static_cast<std::uint64_t>(data.instrument.quantity_step), 8);
    put(bytes, 40, data.events.size(), 8);
    std::uint64_t market_count = 0;
    for (auto& e : data.events) if (e.kind != EventKind::reset) ++market_count;
    put(bytes, 48, market_count, 8); put(bytes, 56, length - header_size, 8);
    std::memcpy(bytes.data() + 72, "BTCUSDT", 7);
    std::memcpy(bytes.data() + 88, "linear", 6);
    std::copy(data.instrument.metadata_sha256.begin(), data.instrument.metadata_sha256.end(), bytes.begin() + 96);
    std::size_t p = header_size;
    for (const auto& e : data.events) {
        const auto n = record_size + (static_cast<std::size_t>(e.bid_count) + e.ask_count) * 16;
        put(bytes, p, n, 4); bytes[p + 4] = static_cast<std::uint8_t>(e.kind);
        put(bytes, p + 8, e.session, 8); put(bytes, p + 16, e.update_id, 8);
        put(bytes, p + 24, e.sequence, 8); put(bytes, p + 32, e.exchange_ms, 8);
        put(bytes, p + 40, e.matching_ms, 8);
        put(bytes, p + 48, static_cast<std::uint64_t>(e.receive_utc_ns), 8);
        put(bytes, p + 56, static_cast<std::uint64_t>(e.receive_steady_ns), 8);
        put(bytes, p + 64, e.bid_count, 4); put(bytes, p + 68, e.ask_count, 4);
        auto pos = p + record_size;
        for (auto side : {data.bids(e), data.asks(e)}) for (auto l : side) {
            put(bytes, pos, static_cast<std::uint64_t>(l.price), 8);
            put(bytes, pos + 8, static_cast<std::uint64_t>(l.quantity), 8); pos += 16;
        }
        p += n;
    }
    put(bytes, 64, crc32(std::span<const std::uint8_t>(bytes).subspan(header_size)), 4);
    put(bytes, 68, crc32(std::span<const std::uint8_t>(bytes).first(header_size)), 4);
    return bytes;
}

EventData decode_events(std::span<const std::uint8_t> bytes) {
    require(bytes.size() >= header_size && bytes.size() <= file_limit, "Invalid events file size");
    require(std::equal(magic.begin(), magic.end(), bytes.begin()), "Invalid file magic");
    require(get(bytes, 8, 4) == 1 && get(bytes, 12, 4) == header_size, "Unsupported format version/header");
    std::array<std::uint8_t, header_size> header{};
    std::copy_n(bytes.begin(), header_size, header.begin());
    std::fill(header.begin() + 68, header.begin() + 72, 0);
    require(crc32(header) == get(bytes, 68, 4), "Header CRC32 mismatch");
    require(get(bytes, 56, 8) == bytes.size() - header_size, "Payload size mismatch");
    require(crc32(bytes.subspan(header_size)) == get(bytes, 64, 4), "Payload CRC32 mismatch");
    constexpr std::array<std::uint8_t, 16> symbol{'B','T','C','U','S','D','T'};
    constexpr std::array<std::uint8_t, 8> category{'l','i','n','e','a','r'};
    require(std::equal(symbol.begin(), symbol.end(), bytes.begin() + 72) &&
        std::equal(category.begin(), category.end(), bytes.begin() + 88), "Unexpected symbol/category");
    const auto count = get(bytes, 40, 8);
    require(count > 0 && count <= (bytes.size() - header_size) / record_size, "Invalid event count");
    EventData data;
    data.instrument.price_decimals = static_cast<std::uint32_t>(get(bytes, 16, 4));
    data.instrument.quantity_decimals = static_cast<std::uint32_t>(get(bytes, 20, 4));
    data.instrument.tick_size = signed_get(bytes, 24); data.instrument.quantity_step = signed_get(bytes, 32);
    std::copy_n(bytes.begin() + 96, 32, data.instrument.metadata_sha256.begin());
    data.events.reserve(static_cast<std::size_t>(count));
    // Each level belongs to exactly one record; reserve once for all decoded levels.
    const auto level_bytes = bytes.size() - header_size - static_cast<std::size_t>(count) * record_size;
    require(level_bytes % 16 == 0, "Invalid aggregate level size");
    data.levels.reserve(level_bytes / 16);
    std::size_t p = header_size;
    std::uint64_t market_count = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        require(p <= bytes.size() && record_size <= bytes.size() - p, "Truncated event header");
        auto n = get(bytes, p, 4);
        require(bytes[p + 5] == 0 && get(bytes, p + 6, 2) == 0, "Nonzero reserved record fields");
        MarketEvent e;
        e.kind = static_cast<EventKind>(bytes[p + 4]);
        e.session = get(bytes, p + 8, 8); e.update_id = get(bytes, p + 16, 8);
        e.sequence = get(bytes, p + 24, 8); e.exchange_ms = get(bytes, p + 32, 8);
        e.matching_ms = get(bytes, p + 40, 8); e.receive_utc_ns = signed_get(bytes, p + 48);
        e.receive_steady_ns = signed_get(bytes, p + 56);
        e.bid_count = static_cast<std::uint32_t>(get(bytes, p + 64, 4));
        e.ask_count = static_cast<std::uint32_t>(get(bytes, p + 68, 4));
        require(e.bid_count <= level_limit && e.ask_count <= level_limit, "Too many levels in record");
        const auto levels = static_cast<std::uint64_t>(e.bid_count) + e.ask_count;
        require(n == record_size + levels * 16 && n <= bytes.size() - p, "Invalid record length");
        e.first_level = data.levels.size();
        for (std::uint64_t j = 0; j < levels; ++j) {
            const auto offset = p + record_size + static_cast<std::size_t>(j) * 16;
            data.levels.push_back({signed_get(bytes, offset), signed_get(bytes, offset + 8)});
        }
        data.events.push_back(e);
        if (e.kind != EventKind::reset) ++market_count;
        p += static_cast<std::size_t>(n);
    }
    require(p == bytes.size() && market_count == get(bytes, 48, 8), "Trailing bytes or market count mismatch");
    validate_events(data);
    return data;
}
void write_events(const std::filesystem::path& path, const EventData& data) {
    const auto bytes = encode_events(data);
    write_exclusive(path, bytes);
}
EventData read_events(const std::filesystem::path& path) {
    const auto n = std::filesystem::file_size(path);
    require(n >= header_size && n <= file_limit, "Invalid events file size");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot open events file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
    require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(n))), "Cannot read events file");
    require(input.peek() == std::char_traits<char>::eof(), "Events file changed while reading");
    return decode_events(bytes);
}
}
