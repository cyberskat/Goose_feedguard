#include "market_event.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout << "Usage: converter --input capture.jsonl --instrument metadata.json --output events.bin\n"
                     "       converter --verify events.bin\nExisting output is never overwritten.\n";
        return 0;
    }
    try {
        auto path = [](const char* arg) {
            const std::string_view s(arg);
            return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
        };
        feedguard::EventData data;
        if (argc == 3 && std::string_view(argv[1]) == "--verify") data = feedguard::read_events(path(argv[2]));
        else {
            std::filesystem::path input, instrument, output;
            for (int i = 1; i < argc; ++i) {
                const std::string_view key(argv[i]);
                if (i + 1 >= argc) throw std::runtime_error("Missing option value");
                auto value = path(argv[++i]);
                auto set = [&](auto& target) {
                    if (!target.empty()) throw std::runtime_error("Duplicate option");
                    target = std::move(value);
                };
                if (key == "--input") set(input);
                else if (key == "--instrument") set(instrument);
                else if (key == "--output") set(output);
                else throw std::runtime_error("Unknown converter option");
            }
            if (input.empty() || instrument.empty() || output.empty()) throw std::runtime_error("--input, --instrument and --output are required");
            data = feedguard::convert_capture(input, feedguard::read_instrument(instrument));
            feedguard::write_events(output, data);
            const auto decoded = feedguard::read_events(output);
            if (decoded != data) throw std::runtime_error("Binary round-trip mismatch");
        }
        std::uint64_t market = 0, resets = 0;
        for (const auto& e : data.events) e.kind == feedguard::EventKind::reset ? ++resets : ++market;
        std::cout << "verified_events=" << data.events.size() << " market=" << market << " resets=" << resets
                  << " levels=" << data.levels.size() << " price_decimals=" << data.instrument.price_decimals
                  << " quantity_decimals=" << data.instrument.quantity_decimals << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "converter: " << error.what() << '\n'; return 1;
    }
}
