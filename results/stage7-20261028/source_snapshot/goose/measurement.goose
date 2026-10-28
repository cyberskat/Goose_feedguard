import book;

// Native utilities are shared with C++; they contain no order-book logic.
extern fn fg_clock_ns() -> u64;
extern fn fg_clock_frequency() -> u64;
extern fn fg_peak_working_set_bytes() -> u64;
extern fn fg_goose_measurement_release() -> i32;

fn read_measure_input(path: const u8[:], events: Event[>..]&, levels: Level[>..]&) -> Instrument {
    var bytes: u8[>..] = [];
    need(read_file(path, bytes), "cannot read input");
    load_events(bytes, events, levels)
}
fn emit_u64_array(out: u8[>..]&, values: u64[:]) {
    out.push('[');
    for value, i in values { if i > 0 { out.push(','); } format(out, value); }
    out.push(']');
}
fn measure_input(path: const u8[:], mode: const u8[:], sampled: bool) {
    need((mode == "core" || mode == "offline") && (!sampled || mode == "core"), "invalid measurement options");
    need(fg_goose_measurement_release() == 1, "measurement requires an optimized Release build");
    need(fg_clock_ns() > 0 && fg_clock_frequency() > 0, "QPC unavailable");
    var timer_samples: u64[>..] = [];
    if sampled {
        for i in 10000 {
            let begin = fg_clock_ns(); let end = fg_clock_ns();
            need(begin > 0 && end >= begin, "invalid calibration clock");
            timer_samples.push(end - begin);
        }
    }
    var events: Event[>..] = [];
    var levels: Level[>..] = [];
    let pipeline_start = if mode == "offline" { fg_clock_ns() } else { 0 as u64 };
    let m = read_measure_input(path, events, levels);
    var s = BookMeta {};
    var last = BookMeta {};
    var bids: Level[>..<] = [];
    var asks: Level[>..<] = [];
    var lb: Level[>..<] = [];
    var la: Level[>..<] = [];
    var samples: u64[>..] = [];
    let begin = if mode == "core" { fg_clock_ns() } else { pipeline_start };
    for e in events {
        if sampled {
            let start = fg_clock_ns();
            let ok = apply_event(m,s,bids,asks,last,lb,la,e,
                levels[e.first..e.first + e.nb], levels[e.first + e.nb..e.first + e.nb + e.na]);
            let end = fg_clock_ns();
            need(ok, "kernel rejected event");
            need(start > 0 && end >= start, "invalid event clock"); samples.push(end - start);
        } else {
            need(apply_event(m,s,bids,asks,last,lb,la,e,
                levels[e.first..e.first + e.nb], levels[e.first + e.nb..e.first + e.nb + e.na]), "kernel rejected event");
        }
    }
    let end = fg_clock_ns();
    need(begin > 0 && end > begin, "invalid measurement duration");
    let checksum = book_hash(m,s,bids,asks);
    let last_checksum = if last.valid { book_hash(m,last,lb,la) } else { 0 as u64 };
    let peak = fg_peak_working_set_bytes();
    need(peak > 0, "Peak Working Set unavailable");
    var markets = 0;
    for e in events { if e.kind != 3 { markets++; } }
    var out: u8[>..] = [];
    format(out, "{\"schema\":1,\"engine\":\"goose\",\"mode\":\"", mode, "\",\"events\":", events.len,
        ",\"market_events\":", markets, ",\"elapsed_ns\":", end - begin,
        ",\"peak_working_set_bytes\":", peak, ",\"clock_frequency_hz\":", fg_clock_frequency(),
        ",\"checksum\":\""); hex64(out, checksum); out.append("\",\"last_valid_checksum\":");
    if last.valid { out.push('"'); hex64(out,last_checksum); out.push('"'); } else { out.append("null"); }
    format(out, ",\"latency_instrumented\":", sampled, ",\"latencies_ns\":"); emit_u64_array(out,samples);
    out.append(",\"timer_samples_ns\":"); emit_u64_array(out,timer_samples);
    out.append(",\"validation_in_timed_apply\":true,\"checksum_in_timed_loop\":false}");
    print(out);
}
