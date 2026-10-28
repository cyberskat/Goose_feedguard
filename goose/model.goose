// Written against the docs shipped with aardappel/goose v2026.3.
import std;
import os;

struct Instrument { pd: i64, qd: i64, tick: i64, step: i64 }
struct Level { price: i64, quantity: i64 }
struct Event {
    kind: u8, session: u64, u: u64, seq: u64, ts: u64, cts: u64,
    utc: i64, steady: i64, first: i64, nb: i64, na: i64
}
struct BookMeta {
    valid: bool = false, session: u64 = 0, u: u64 = 0, seq: u64 = 0,
    last_steady: i64 = 0
}

fn need(ok: bool, message: const u8[:]) {
    if !ok { write_stderr(str("orderbook: ", message, "\n")); exit(1); }
}
fn hash_byte(h: u64, b: u8) -> u64 { (h ^ b) * 1099511628211 }
fn hash_bytes(h: u64, bytes: const u8[:]) -> u64 {
    var value = h;
    for b in bytes { value = hash_byte(value, b); }
    value
}
fn hash_int(h: u64, n: u64, width: i64) -> u64 {
    var value = h;
    for i in width { value = hash_byte(value, (n >> (8 * i)) as! u8); }
    value
}
fn book_hash(m: Instrument, s: BookMeta, bids: Level[:], asks: Level[:]) -> u64 {
    var h: u64 = 14695981039346656037;
    h = hash_bytes(h, "BFGBOOK1");
    h = hash_int(h, m.pd as u64, 4); h = hash_int(h, m.qd as u64, 4);
    h = hash_int(h, m.tick as u64, 8); h = hash_int(h, m.step as u64, 8);
    h = hash_byte(h, if s.valid { 1 } else { 0 });
    h = hash_int(h, s.session, 8); h = hash_int(h, s.u, 8); h = hash_int(h, s.seq, 8);
    h = hash_int(h, bids.len as u64, 8); h = hash_int(h, asks.len as u64, 8);
    for l in bids { h = hash_int(h, l.price as! u64, 8); h = hash_int(h, l.quantity as! u64, 8); }
    for l in asks { h = hash_int(h, l.price as! u64, 8); h = hash_int(h, l.quantity as! u64, 8); }
    h
}
fn trace_hash(h: u64, e: Event, checksum: u64) -> u64 {
    var value = hash_byte(h, e.kind);
    value = hash_int(value, e.session, 8); value = hash_int(value, e.u, 8);
    value = hash_int(value, e.seq, 8); hash_int(value, checksum, 8)
}
fn hex64(out: u8[>..]&, value: u64) { format_uint(out, value, 16, 16, '0'); }
