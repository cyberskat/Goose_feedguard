#include "order_book.hpp"
#include <charconv>
#include <iostream>

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout << "Usage: book_cpp --input events.bin [--engine map|vector]\n"
                     "       book_cpp --input events.bin --engine map|vector --benchmark --repeat 50\n"
                     "       book_cpp --input events.bin --engine map|vector --measure core|offline [--latencies]\n"
                     "JSON replay or preliminary core-only benchmark (Release required).\n";
        return 0;
    }
    try {
        std::filesystem::path path;
        auto engine = feedguard::Engine::map;
        bool benchmark = false, engine_set = false, repeat_set = false, input_set = false;
        std::string_view measure;
        bool latencies = false;
        std::uint64_t repeat = 50;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key(argv[i]);
            if (key == "--latencies") {
                if (latencies) throw std::runtime_error("Duplicate --latencies");
                latencies = true; continue;
            }
            if (key == "--benchmark") {
                if (benchmark) throw std::runtime_error("Duplicate --benchmark");
                benchmark = true; continue;
            }
            if (++i >= argc) throw std::runtime_error("Missing option value");
            const std::string_view arg(argv[i]);
            if (key == "--input" && !input_set) {
                path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(arg.data()), arg.size()));
                input_set = true;
            }
            else if (key == "--engine" && !engine_set) {
                if (arg == "map") engine = feedguard::Engine::map;
                else if (arg == "vector") engine = feedguard::Engine::vector;
                else throw std::runtime_error("Engine must be map or vector");
                engine_set = true;
            } else if (key == "--repeat" && !repeat_set) {
                const auto [end, error] = std::from_chars(arg.data(), arg.data() + arg.size(), repeat);
                if (error != std::errc{} || end != arg.data() + arg.size() || repeat == 0 || repeat > 1000)
                    throw std::runtime_error("--repeat must be in 1..1000");
                repeat_set = true;
            } else if (key == "--measure" && measure.empty()) {
                if (arg != "core" && arg != "offline") throw std::runtime_error("Measure mode must be core or offline");
                measure = arg;
            } else throw std::runtime_error("Unknown or duplicate option");
        }
        if (path.empty() || (repeat_set && !benchmark)) throw std::runtime_error("--input required; --repeat requires --benchmark");
        if ((benchmark && !measure.empty()) || (latencies && measure != "core")) throw std::runtime_error("Incompatible measurement options");
#ifndef NDEBUG
        if (benchmark || !measure.empty()) throw std::runtime_error("Benchmark requires a Release build");
#endif
        if (!measure.empty()) {
            std::cout << feedguard::measure_run(path, engine, measure, latencies) << '\n';
            if (!std::cout) throw std::runtime_error("Cannot write measurement JSON");
            return 0;
        }
        const auto data = feedguard::read_events(path);
        if (benchmark) std::cout << feedguard::benchmark_json(feedguard::benchmark_core(data, engine, repeat)) << '\n';
        else {
            const auto result = engine == feedguard::Engine::map ? feedguard::replay_map(data) : feedguard::replay_vector(data);
            std::cout << feedguard::replay_json(result, engine) << '\n';
        }
        if (!std::cout) throw std::runtime_error("Cannot write replay JSON");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "book_cpp: " << error.what() << '\n'; return 1;
    }
}
