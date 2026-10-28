#include "order_book.hpp"
#include "measurement.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace feedguard {
namespace {
template<class T> void number(std::string& out, T n) {
    char text[32];
    auto [end, error] = std::to_chars(text, text + sizeof(text), n);
    if (error != std::errc{}) throw std::runtime_error("Cannot encode measurement integer");
    out.append(text, end);
}
void array(std::string& out, const std::vector<std::uint64_t>& values) {
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out += ','; number(out, values[i]); }
    out += ']';
}
template<class Book> std::string run(const std::filesystem::path& path, Engine engine,
                                     std::string_view mode, bool latencies) {
    if (!fg_clock_ns() || !fg_clock_frequency()) throw std::runtime_error("QPC unavailable");
    std::vector<std::uint64_t> timer_samples;
    if (latencies) {
        timer_samples.resize(10000);
        for (auto& sample : timer_samples) {
            auto start = fg_clock_ns(); auto end = fg_clock_ns();
            if (!start || end < start) throw std::runtime_error("Invalid calibration clock");
            sample = end - start;
        }
    }
    const auto pipeline_start = mode == "offline" ? fg_clock_ns() : 0;
    const auto data = read_events(path);
    Book book(data.instrument);
    std::vector<std::uint64_t> samples;
    if (latencies) samples.resize(data.events.size());
    const auto start = mode == "core" ? fg_clock_ns() : pipeline_start;
    for (std::size_t i = 0; i < data.events.size(); ++i) {
        const auto& e = data.events[i];
        if (latencies) {
            auto begin = fg_clock_ns();
            book.apply(e, data.bids(e), data.asks(e));
            auto end = fg_clock_ns();
            if (!begin || end < begin) throw std::runtime_error("Invalid event clock");
            samples[i] = end - begin;
        } else book.apply(e, data.bids(e), data.asks(e));
    }
    const auto end = fg_clock_ns();
    if (!start || end <= start) throw std::runtime_error("Invalid measurement duration");
    const auto checksum = book.checksum();
    const auto& last = book.last_valid_before_reset();
    const auto peak = fg_peak_working_set_bytes();
    if (!peak) throw std::runtime_error("Peak Working Set unavailable");
    const auto markets = std::count_if(data.events.begin(), data.events.end(), [](const auto& e) { return e.kind != EventKind::reset; });
    std::string out;
    out.reserve(1024 + samples.size() * 8 + timer_samples.size() * 5);
    out += "{\"schema\":1,\"engine\":\"" + std::string(engine_name(engine)) + "\",\"mode\":\"" + std::string(mode) + '"';
    out += ",\"events\":"; number(out, data.events.size());
    out += ",\"market_events\":"; number(out, markets);
    out += ",\"elapsed_ns\":"; number(out, end - start);
    out += ",\"peak_working_set_bytes\":"; number(out, peak);
    out += ",\"clock_frequency_hz\":"; number(out, fg_clock_frequency());
    out += ",\"checksum\":\"" + hex_checksum(checksum) + "\",\"last_valid_checksum\":";
    out += last ? '"' + hex_checksum(state_checksum(data.instrument, *last)) + '"' : "null";
    out += ",\"latency_instrumented\":"; out += latencies ? "true" : "false";
    out += ",\"latencies_ns\":"; array(out, samples);
    out += ",\"timer_samples_ns\":"; array(out, timer_samples);
    out += ",\"validation_in_timed_apply\":true,\"checksum_in_timed_loop\":false}";
    return out;
}
}
std::string measure_run(const std::filesystem::path& input, Engine engine, std::string_view mode, bool latencies) {
    if ((mode != "core" && mode != "offline") || (latencies && mode != "core"))
        throw std::runtime_error("--measure must be core|offline; --latencies requires core");
    if (engine == Engine::map) return run<MapBook>(input, engine, mode, latencies);
    return run<VectorBook>(input, engine, mode, latencies);
}
}
