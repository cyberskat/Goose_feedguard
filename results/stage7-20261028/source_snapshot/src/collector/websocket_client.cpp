#include "collector.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/ssl.h>
#include <csignal>
#include <functional>
#include <iostream>
namespace feedguard {
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
using Error = boost::system::error_code;
using namespace std::chrono_literals;
constexpr char host[] = "stream.bybit.com";
constexpr char target[] = "/v5/public/linear";
constexpr std::string_view subscribe = R"({"op":"subscribe","args":["orderbook.50.BTCUSDT"]})";
constexpr std::string_view ping = R"({"op":"ping"})";

class Session : public std::enable_shared_from_this<Session> {
    tcp::resolver resolver_;
    ws::stream<beast::ssl_stream<beast::tcp_stream>> socket_;
    net::steady_timer heartbeat_, deadline_, close_deadline_;
    beast::flat_buffer buffer_{1024 * 1024};
    simdjson::dom::parser parser_;
    Journal& journal_;
    std::uint64_t id_;
    SessionGate gate_;
    bool ended_ = false, stopping_ = false;
    std::chrono::steady_clock::time_point last_market_{};
    std::function<bool()> received_;
    std::function<void(std::string)> done_;
    auto self() { return shared_from_this(); }
    void finish(std::string why) {
        if (ended_) return;
        ended_ = true;
        gate_.reset();
        heartbeat_.cancel(); deadline_.cancel(); close_deadline_.cancel(); resolver_.cancel();
        Error ignored;
        beast::get_lowest_layer(socket_).socket().cancel(ignored);
        beast::get_lowest_layer(socket_).socket().close(ignored);
        journal_.event(id_, "disconnect", why);
        done_(std::move(why));
    }
    bool failed(Error error, std::string_view stage) {
        if (ended_) return true;
        if (!error) return false;
        finish(std::string(stage) + ": " + error.message());
        return true;
    }
    void watch_deadline() {
        deadline_.async_wait([s = self()](Error ec) {
            if (ec || s->ended_ || s->stopping_) return;
            const auto limit = s->last_market_ + 30s;
            if (!s->gate_.ready || std::chrono::steady_clock::now() >= limit) {
                s->finish("connection/snapshot/market feed timeout"); return;
            }
            s->deadline_.expires_at(limit);
            s->watch_deadline();
        });
    }
    void read() {
        socket_.async_read(buffer_, [s = self()](Error error, std::size_t) {
            const auto utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto steady = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (s->failed(error, "read") || s->stopping_) return;
            if (!s->socket_.got_text()) { s->finish("binary message rejected"); return; }
            auto data = s->buffer_.data();
            std::string_view wire(static_cast<const char*>(data.data()), data.size());
            const auto kind = classify(s->parser_, wire);
            if (kind == MessageKind::invalid) {
                s->journal_.event(s->id_, "invalid", wire);
                s->finish("invalid message; new snapshot required"); return;
            }
            const bool accepted = s->gate_.accept(kind);
            s->journal_.message(s->id_, kind == MessageKind::snapshot ? "snapshot" :
                kind == MessageKind::delta ? "delta" : "control", wire, utc, steady, accepted);
            s->buffer_.consume(s->buffer_.size());
            if (kind == MessageKind::rejected) { s->finish("subscription rejected"); return; }
            if (kind == MessageKind::delta && !accepted) { s->finish("delta before snapshot"); return; }
            if (accepted) {
                s->last_market_ = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(steady));
                if (s->received_()) { s->stop(); return; }
            }
            s->read();
        });
    }
    void heartbeat() {
        heartbeat_.expires_after(20s);
        heartbeat_.async_wait([s = self()](Error ec) {
            if (ec || s->ended_ || s->stopping_) return;
            s->socket_.async_write(net::buffer(ping.data(), ping.size()), [s](Error error, std::size_t) {
                if (!s->failed(error, "heartbeat") && !s->stopping_) s->heartbeat();
            });
        });
    }
public:
    Session(net::io_context& io, ssl::context& tls, Journal& journal, std::uint64_t id,
            std::function<bool()> received, std::function<void(std::string)> done)
        : resolver_(io), socket_(io, tls), heartbeat_(io), deadline_(io), close_deadline_(io),
          journal_(journal), id_(id), received_(std::move(received)), done_(std::move(done)) {
        buffer_.reserve(64 * 1024);
    }
    void start() {
        journal_.event(id_, "session_start", host);
        deadline_.expires_after(30s);
        watch_deadline();
        socket_.next_layer().set_verify_mode(ssl::verify_peer);
        socket_.next_layer().set_verify_callback(ssl::host_name_verification(host));
        if (!SSL_set_tlsext_host_name(socket_.next_layer().native_handle(), host)) {
            finish("Cannot configure TLS SNI"); return;
        }
        resolver_.async_resolve(host, "443", [s = self()](Error ec, tcp::resolver::results_type results) {
            if (s->failed(ec, "resolve")) return;
            beast::get_lowest_layer(s->socket_).expires_after(15s);
            beast::get_lowest_layer(s->socket_).async_connect(results, [s](Error error, const auto&) {
                if (s->failed(error, "connect")) return;
                Error option_error;
                beast::get_lowest_layer(s->socket_).socket().set_option(tcp::no_delay(true), option_error);
                if (s->failed(option_error, "TCP_NODELAY")) return;
                s->socket_.next_layer().async_handshake(ssl::stream_base::client, [s](Error tls_error) {
                    if (s->failed(tls_error, "TLS")) return;
                    beast::get_lowest_layer(s->socket_).expires_never();
                    s->socket_.set_option(ws::stream_base::timeout::suggested(beast::role_type::client));
                    s->socket_.read_message_max(1024 * 1024);
                    s->socket_.async_handshake(host, target, [s](Error websocket_error) {
                        if (s->failed(websocket_error, "WebSocket handshake")) return;
                        s->socket_.text(true);
                        s->socket_.async_write(net::buffer(subscribe.data(), subscribe.size()),
                            [s](Error write_error, std::size_t) {
                                if (s->failed(write_error, "subscribe")) return;
                                s->heartbeat(); s->read();
                            });
                    });
                });
            });
        });
    }
    void stop() {
        if (ended_ || stopping_) return;
        stopping_ = true;
        heartbeat_.cancel(); deadline_.cancel(); resolver_.cancel();
        if (!socket_.is_open()) { finish("stopped before handshake"); return; }
        close_deadline_.expires_after(3s);
        close_deadline_.async_wait([s = self()](Error ec) { if (!ec) s->finish("close timeout"); });
        socket_.async_close(ws::close_code::normal, [s = self()](Error ec) {
            s->finish(ec ? "close: " + ec.message() : "normal close");
        });
    }
};

int run_collector(const CollectorOptions& options) {
    net::io_context io(1);
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    if (!options.ca_file.empty()) tls.load_verify_file(options.ca_file);
#ifdef _WIN32
    else {
        auto roots = std::filesystem::path(FEEDGUARD_CA_BUNDLE);
        if (std::filesystem::exists(roots)) tls.load_verify_file(roots.string());
    }
#endif
    if (!SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_2_VERSION))
        throw std::runtime_error("Cannot require TLS 1.2");
    if (!SSL_CTX_set1_groups_list(tls.native_handle(), "X25519:P-256"))
        throw std::runtime_error("Cannot configure TLS groups X25519/P-256");
    Journal journal(options.output);
    net::steady_timer retry(io), timeout(io);
    net::signal_set signals(io, SIGINT, SIGTERM);
    std::shared_ptr<Session> session;
    bool stopping = false, timed_out = false, interrupted = false;
    std::uint64_t received = 0, sessions = 0;
    unsigned failures = 0;
    std::function<void()> connect;
    auto stop = [&] {
        if (stopping) return;
        stopping = true;
        retry.cancel(); timeout.cancel(); signals.cancel();
        if (session) session->stop();
    };
    connect = [&] {
        if (stopping) return;
        const auto before = received;
        session = std::make_shared<Session>(io, tls, journal, ++sessions,
            [&] { if (++received == options.count) { stop(); return true; } return false; },
            [&, before](std::string why) {
                if (stopping) return;
                std::cerr << "session " << sessions << ": " << why << '\n';
                if (received > before) failures = 0;
                const auto delay = backoff_seconds(failures);
                if (failures < 5) ++failures;
                retry.expires_after(std::chrono::seconds(delay));
                retry.async_wait([&](Error ec) { if (!ec) connect(); });
            });
        session->start();
    };
    signals.async_wait([&](Error ec, int) { if (!ec) { interrupted = true; stop(); } });
    timeout.expires_after(std::chrono::seconds(options.timeout));
    timeout.async_wait([&](Error ec) { if (!ec) { timed_out = true; stop(); } });
    connect();
    io.run();
    journal.event(sessions, "collector_stop", "accepted=" + std::to_string(received) +
        (timed_out ? "; timeout" : interrupted ? "; interrupted" : "; count reached"));
    journal.finish();
    std::cout << "market_messages=" << received << " sessions=" << sessions
              << " output=" << options.output.string() << '\n';
    return timed_out ? 2 : interrupted ? 130 : received == options.count ? 0 : 1;
}
}
