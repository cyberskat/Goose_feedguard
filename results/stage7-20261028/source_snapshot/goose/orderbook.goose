import book;
import self_tests;
import measurement;

fn main() {
    let a = args();
    if a.len == 2 && a[1] == "--self-test" {
        let checks = self_tests();
        print("{\"self_tests_passed\":", checks, "}"); return;
    }
    if a.len == 1 || (a.len == 2 && a[1] == "--help") {
        print("Usage: orderbook --input events.bin [--trace]\n       orderbook --self-test\n       orderbook --input events.bin --measure core|offline [--latencies]"); return;
    }
    if a.len >= 5 && a[1] == "--input" && a[3] == "--measure" {
        need(a.len == 5 || (a.len == 6 && a[5] == "--latencies"), "invalid measurement arguments");
        measure_input(a[2],a[4],a.len == 6); return;
    }
    need((a.len == 3 || a.len == 4) && a[1] == "--input", "usage: orderbook --input events.bin [--trace]");
    let trace = a.len == 4;
    if trace { need(a[3] == "--trace", "unknown option"); }
    var bytes: u8[>..] = [];
    need(read_file(a[2], bytes), "cannot read input");
    var events: Event[>..] = [];
    var levels: Level[>..] = [];
    let m = load_events(bytes, events, levels);
    var s = BookMeta {};
    var last = BookMeta {};
    var bids: Level[>..<] = [];
    var asks: Level[>..<] = [];
    var last_bids: Level[>..<] = [];
    var last_asks: Level[>..<] = [];
    var history = hash_bytes(14695981039346656037, "BFGTRACE1");
    var markets = 0;
    var resets = 0;
    for e, i in events {
        need(apply_event(m, s, bids, asks, last, last_bids, last_asks, e,
            levels[e.first..e.first + e.nb], levels[e.first + e.nb..e.first + e.nb + e.na]), "kernel rejected event");
        if e.kind == 3 { resets++; } else { markets++; }
        history = trace_hash(history, e, book_hash(m, s, bids, asks));
        if trace {
            var line: u8[>..] = [];
            emit_report(line, m, s, bids, asks, last, last_bids, last_asks, i + 1, markets, resets, history);
            print(line);
        }
    }
    if !trace {
        var out: u8[>..] = [];
        emit_report(out, m, s, bids, asks, last, last_bids, last_asks, events.len, markets, resets, history);
        print(out);
    }
}
