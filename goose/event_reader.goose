import model;
import binary;

fn u32_at(data: const u8[:], at: i64) -> u32 {
    let value, ok = binary::read_u32le(data, at);
    need(ok, "truncated u32"); value
}
fn u64_at(data: const u8[:], at: i64) -> u64 {
    need(binary::range_ok(data, at, 8), "truncated u64");
    var out: u64 = 0;
    for i in 8 { out |= (data[at + i] as u64) << (8 * i); }
    out
}
fn i64_at(data: const u8[:], at: i64) -> i64 { u64_at(data, at) as! i64 }
fn crc32(data: const u8[:], zero_header_crc: bool) -> u32 {
    var table: u32[256] = [0; 256];
    for i in 256 {
        var x = i as u32;
        for k in 8 { x = (x >> 1) ^ (if (x & 1) != 0 { 0xedb88320 as u32 } else { 0 as u32 }); }
        table[i] = x;
    }
    var crc: u32 = 0xffffffff;
    for b, i in data {
        let byte: u8 = if zero_header_crc && i >= 68 && i < 72 { 0 } else { b };
        crc = table[((crc ^ byte) & 255) as i64] ^ (crc >> 8);
    }
    crc ^ 0xffffffff
}
fn valid_side(m: Instrument, levels: Level[:], snapshot: bool, bid: bool) -> bool {
    var prices: i64[>..<] = [];
    for l, i in levels {
        if l.price <= 0 || l.quantity < 0 || (snapshot && l.quantity == 0) { return false; }
        if l.price % m.tick != 0 || l.quantity % m.step != 0 { return false; }
        if snapshot && i > 0 {
            if bid && levels[i - 1].price <= l.price { return false; }
            if !bid && levels[i - 1].price >= l.price { return false; }
        }
        prices.push(l.price);
    }
    sort(prices);
    for i in 1..prices.len { if prices[i - 1] == prices[i] { return false; } }
    true
}
fn load_events(data: const u8[:], events: Event[>..]&, levels: Level[>..]&) -> Instrument {
    need(data.len >= 128 && data.len <= 1073741824, "invalid file size");
    need(data[..8] == "BFGDEVT1" && u32_at(data, 8) == 1 && u32_at(data, 12) == 128, "unsupported binary format");
    need(crc32(data[..128], true) == u32_at(data, 68), "header CRC32 mismatch");
    need(u64_at(data, 56) == (data.len - 128) as u64, "payload size mismatch");
    need(crc32(data[128..], false) == u32_at(data, 64), "payload CRC32 mismatch");
    need(data[72..79] == "BTCUSDT" && data[88..94] == "linear", "unexpected symbol/category");
    for i in 79..88 { need(data[i] == 0, "invalid symbol padding"); }
    for i in 94..96 { need(data[i] == 0, "invalid category padding"); }
    let m = Instrument { u32_at(data, 16), u32_at(data, 20), i64_at(data, 24), i64_at(data, 32) };
    need(m.pd <= 18 && m.qd <= 18 && m.tick > 0 && m.step > 0, "invalid instrument");
    let count = u64_at(data, 40);
    need(count > 0 && count <= ((data.len - 128) / 72) as u64, "invalid event count");
    var p = 128;
    var markets: u64 = 0;
    var session: u64 = 0;
    var ready = false;
    var last_steady: i64 = 0;
    for index in count as i64 {
        need(binary::range_ok(data, p, 72), "truncated event header");
        let n = u32_at(data, p) as i64;
        need(data[p + 5] == 0 && data[p + 6] == 0 && data[p + 7] == 0, "nonzero reserved record fields");
        let e = Event { data[p + 4], u64_at(data, p + 8), u64_at(data, p + 16), u64_at(data, p + 24),
            u64_at(data, p + 32), u64_at(data, p + 40), i64_at(data, p + 48), i64_at(data, p + 56),
            levels.len, u32_at(data, p + 64), u32_at(data, p + 68) };
        need(e.session > 0 && e.utc > 0 && e.steady >= 0, "invalid session/timestamp");
        need(e.nb <= 4096 && e.na <= 4096, "too many level updates");
        need(n == 72 + (e.nb + e.na) * 16 && binary::range_ok(data, p, n), "invalid event length");
        if e.kind == 3 {
            need(e.session >= session && e.nb == 0 && e.na == 0 && e.u == 0 && e.seq == 0 &&
                 e.ts == 0 && e.cts == 0 && e.steady == 0, "invalid reset");
            session = e.session; ready = false;
        } else {
            need(e.kind == 1 || e.kind == 2, "unknown event kind");
            need(e.session == session && e.u > 0 && e.ts > 0 && e.steady >= last_steady, "invalid market metadata");
            if e.kind == 1 { need(e.nb <= 50 && e.na <= 50, "snapshot depth exceeds 50"); ready = true; }
            else { need(ready && e.u != 1, "delta before snapshot or unexpected restart"); }
            last_steady = e.steady;
            markets += 1;
        }
        for j in e.nb + e.na {
            levels.push(Level { i64_at(data, p + 72 + j * 16), i64_at(data, p + 80 + j * 16) });
        }
        need(valid_side(m, levels[e.first..e.first + e.nb], e.kind == 1, true), "invalid BID changes");
        need(valid_side(m, levels[e.first + e.nb..e.first + e.nb + e.na], e.kind == 1, false), "invalid ASK changes");
        events.push(e);
        p += n;
    }
    need(p == data.len && markets == u64_at(data, 48), "trailing bytes or market count mismatch");
    m
}
