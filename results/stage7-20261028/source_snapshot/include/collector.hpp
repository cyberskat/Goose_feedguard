#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <simdjson.h>
namespace feedguard {
struct CollectorOptions {
    std::uint64_t count = 10000;
    unsigned timeout = 600;
    std::filesystem::path output = "data/raw/btcusdt.jsonl";
    std::string ca_file;
};
CollectorOptions parse_options(int argc, char** argv);
enum class MessageKind { invalid, control, rejected, snapshot, delta };
MessageKind classify(simdjson::dom::parser& parser, std::string_view wire);
std::string json_quote(std::string_view text);
unsigned backoff_seconds(unsigned failures);
struct SessionGate {
    bool ready = false;
    void reset() { ready = false; }
    bool accept(MessageKind kind) {
        if (kind == MessageKind::snapshot) ready = true;
        return ready && (kind == MessageKind::snapshot || kind == MessageKind::delta);
    }
};
class Journal {
public:
    explicit Journal(const std::filesystem::path& path);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    void message(std::uint64_t session, std::string_view kind, std::string_view wire,
                 std::int64_t utc_ns, std::int64_t steady_ns, bool accepted);
    void event(std::uint64_t session, std::string_view kind, std::string_view detail);
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int run_collector(const CollectorOptions& options);
}
