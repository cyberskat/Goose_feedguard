#include "collector.hpp"
#include "feedguard_config.hpp"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace feedguard {
CollectorOptions parse_options(int argc, char** argv) {
    CollectorOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string_view key(argv[i]);
        if (i + 1 >= argc) throw std::runtime_error("Missing value for " + std::string(key));
        std::string_view value(argv[++i]);
        if (key == "--output") options.output = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
        else if (key == "--ca-file") options.ca_file = value;
        else if (key == "--count" || key == "--timeout") {
            std::uint64_t n = 0;
            auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (error != std::errc{} || end != value.data() + value.size() || n == 0)
                throw std::runtime_error("Expected positive integer for " + std::string(key));
            if (key == "--count") options.count = n;
            else {
                if (n > 86400) throw std::runtime_error("--timeout must be <= 86400 seconds");
                options.timeout = static_cast<unsigned>(n);
            }
        } else throw std::runtime_error("Unknown argument " + std::string(key));
    }
    return options;
}
MessageKind classify(simdjson::dom::parser& parser, std::string_view wire) {
    simdjson::dom::element doc;
    if (parser.parse(wire.data(), wire.size()).get(doc)) return MessageKind::invalid;
    simdjson::dom::object obj;
    if (doc.get(obj)) return MessageKind::invalid;
    std::string_view topic;
    if (doc["topic"].get(topic)) {
        std::string_view op;
        bool success = true;
        if (!doc["op"].get(op) && op == "subscribe" &&
            !doc["success"].get(success) && !success) return MessageKind::rejected;
        return MessageKind::control;
    }
    if (topic != feedguard::topic) return MessageKind::control;
    std::string_view type, symbol;
    std::uint64_t u = 0, seq = 0, ts = 0;
    simdjson::dom::array bids, asks;
    if (doc["type"].get(type) || doc["ts"].get(ts) ||
        doc["data"]["s"].get(symbol) || symbol != feedguard::symbol ||
        doc["data"]["u"].get(u) || doc["data"]["seq"].get(seq) ||
        doc["data"]["b"].get(bids) || doc["data"]["a"].get(asks)) return MessageKind::invalid;
    for (auto levels : {bids, asks}) {
        for (auto level : levels) {
            simdjson::dom::array pair;
            if (level.get(pair) || pair.size() != 2) return MessageKind::invalid;
            for (auto field : pair) {
                std::string_view decimal;
                if (field.get(decimal) || decimal.empty()) return MessageKind::invalid;
                bool digit = false, dot = false;
                for (char c : decimal) {
                    if (c >= '0' && c <= '9') digit = true;
                    else if (c == '.' && !dot) dot = true;
                    else return MessageKind::invalid;
                }
                if (!digit || decimal.front() == '.' || decimal.back() == '.') return MessageKind::invalid;
            }
        }
    }
    if (type == "snapshot") return MessageKind::snapshot;
    if (type == "delta" && u != 1) return MessageKind::delta;
    return MessageKind::invalid;
}
std::string json_quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32 || c >= 127) {
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    out += '"';
    return out;
}
unsigned backoff_seconds(unsigned failures) { return std::min(1u << std::min(failures, 5u), 30u); }
struct Journal::Impl {
    std::FILE* file = nullptr;
    std::vector<char> buffer = std::vector<char>(1024 * 1024);
    std::string line;
    template<class Integer> void number(Integer value) {
        char digits[32];
        const auto [end, error] = std::to_chars(digits, digits + sizeof(digits), value);
        if (error != std::errc{}) throw std::runtime_error("Cannot encode journal integer");
        line.append(digits, end);
    }
    void write() {
        line += '\n';
        if (std::fwrite(line.data(), 1, line.size(), file) != line.size())
            throw std::runtime_error("JSONL write failed");
    }
};
Journal::Journal(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    int fd = -1;
#ifdef _WIN32
    const auto error = _wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                                _SH_DENYWR, _S_IREAD | _S_IWRITE);
    if (error || fd < 0) throw std::runtime_error("Cannot create output (existing files are never overwritten)");
    impl_->file = _fdopen(fd, "wb");
    if (!impl_->file) _close(fd);
#else
    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) throw std::runtime_error("Cannot create output (existing files are never overwritten)");
    impl_->file = fdopen(fd, "wb");
    if (!impl_->file) ::close(fd);
#endif
    if (!impl_->file) throw std::runtime_error("Cannot open output stream");
    std::setvbuf(impl_->file, impl_->buffer.data(), _IOFBF, impl_->buffer.size());
    impl_->line.reserve(65536);
}
Journal::~Journal() { if (impl_->file) std::fclose(impl_->file); }
void Journal::finish() {
    auto* file = impl_->file;
    impl_->file = nullptr;
    if (file && std::fclose(file)) throw std::runtime_error("JSONL flush/close failed");
}
void Journal::message(std::uint64_t session, std::string_view kind, std::string_view wire,
                      std::int64_t utc, std::int64_t steady, bool accepted) {
    auto& line = impl_->line;
    line.clear();
    line += "{\"schema\":1,\"session\":";
    impl_->number(session);
    line += ",\"kind\":" + json_quote(kind);
    line += ",\"recv_utc_ns\":";
    impl_->number(utc);
    line += ",\"recv_steady_ns\":";
    impl_->number(steady);
    line += accepted ? ",\"accepted\":true,\"raw\":" : ",\"accepted\":false,\"raw\":";
    // JSON whitespace can contain newlines; keep one physical line per record.
    if (wire.find_first_of("\r\n") == std::string_view::npos) line.append(wire);
    else for (char c : wire) { if (c != '\r' && c != '\n') line += c; }
    line += '}';
    impl_->write();
}
void Journal::event(std::uint64_t session, std::string_view kind, std::string_view detail) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    impl_->line = "{\"schema\":1,\"session\":" + std::to_string(session) +
        ",\"kind\":" + json_quote(kind) + ",\"recv_utc_ns\":" + std::to_string(ns) +
        ",\"detail\":" + json_quote(detail) + '}';
    impl_->write();
}
}
