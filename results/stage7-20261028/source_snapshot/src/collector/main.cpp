#include "collector.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout << "Bybit FeedGuard collector (public linear BTCUSDT, depth 50)\n"
                  << "Usage: collector --count 10000 --output data/raw/btcusdt.jsonl "
                     "[--timeout 600] [--ca-file PEM]\n"
                  << "Existing output files are never overwritten. Timeout is in seconds.\n";
        return 0;
    }
    try { return feedguard::run_collector(feedguard::parse_options(argc, argv)); }
    catch (const std::exception& error) {
        std::cerr << "collector: " << error.what() << '\n';
        return 1;
    }
}
