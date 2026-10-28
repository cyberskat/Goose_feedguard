import event_reader;

fn valid_event(m: Instrument, s: BookMeta, e: Event, bids: Level[:], asks: Level[:]) -> bool {
    if m.pd < 0 || m.pd > 18 || m.qd < 0 || m.qd > 18 || m.tick <= 0 || m.step <= 0 { return false; }
    if e.session == 0 || e.utc <= 0 || e.steady < 0 || e.nb != bids.len || e.na != asks.len { return false; }
    if bids.len > 4096 || asks.len > 4096 { return false; }
    if e.kind == 3 {
        return e.session >= s.session && bids.len == 0 && asks.len == 0 && e.u == 0 &&
            e.seq == 0 && e.ts == 0 && e.cts == 0 && e.steady == 0;
    }
    if e.kind != 1 && e.kind != 2 { return false; }
    if e.u == 0 || e.ts == 0 || e.steady < s.last_steady { return false; }
    if s.session != 0 && e.session != s.session { return false; }
    if e.kind == 2 && (!s.valid || e.u == 1) { return false; }
    if e.kind == 1 && (bids.len > 50 || asks.len > 50) { return false; }
    valid_side(m, bids, e.kind == 1, true) && valid_side(m, asks, e.kind == 1, false)
}
fn level_position(side: Level[:], price: i64, bid: bool) -> i64 {
    var lo = 0;
    var hi = side.len;
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        let before = if bid { side[mid].price > price } else { side[mid].price < price };
        if before { lo = mid + 1; } else { hi = mid; }
    }
    lo
}
fn apply_side(side: Level[>..<]&, changes: Level[:], bid: bool) {
    for l in changes {
        if l.quantity == 0 {
            let at = level_position(side, l.price, bid);
            if at < side.len && side[at].price == l.price { remove_at(side, at); }
        }
    }
    for l in changes {
        if l.quantity != 0 {
            let at = level_position(side, l.price, bid);
            if at < side.len && side[at].price == l.price { side[at].quantity = l.quantity; }
            else { insert_at(side, at, l); }
        }
    }
}
fn apply_event(m: Instrument, s: BookMeta&, bids: Level[>..<]&, asks: Level[>..<]&,
    last: BookMeta&, last_bids: Level[>..<]&, last_asks: Level[>..<]&,
    e: Event, bid_changes: Level[:], ask_changes: Level[:]) -> bool {
    if !valid_event(m, s, e, bid_changes, ask_changes) { return false; }
    if e.kind == 3 {
        if s.valid {
            last_bids.clear(); last_bids.append(bids);
            last_asks.clear(); last_asks.append(asks);
            last = s;
        }
        bids.clear(); asks.clear(); s.valid = false;
        s.session = e.session; s.u = 0; s.seq = 0;
        return true;
    }
    if e.kind == 1 {
        bids.clear(); bids.append(bid_changes); asks.clear(); asks.append(ask_changes);
    } else {
        apply_side(bids, bid_changes, true); apply_side(asks, ask_changes, false);
    }
    s.valid = true; s.session = e.session; s.u = e.u; s.seq = e.seq; s.last_steady = e.steady;
    true
}

fn emit_level(out: u8[>..]&, l: Level) { format(out, "[", l.price, ",", l.quantity, "]"); }
fn emit_state(out: u8[>..]&, m: Instrument, s: BookMeta, bids: Level[:], asks: Level[:]) {
    format(out, "{\"valid\":", s.valid, ",\"session\":", s.session, ",\"u\":", s.u, ",\"seq\":", s.seq, ",\"checksum\":\"");
    hex64(out, book_hash(m, s, bids, asks));
    out.append("\",\"best_bid\":");
    if s.valid && bids.len > 0 { emit_level(out, bids[0]); } else { out.append("null"); }
    out.append(",\"best_ask\":");
    if s.valid && asks.len > 0 { emit_level(out, asks[0]); } else { out.append("null"); }
    out.append(",\"spread\":");
    if s.valid && bids.len > 0 && asks.len > 0 { format(out, asks[0].price - bids[0].price); }
    else { out.append("null"); }
    out.append(",\"bids\":[");
    for l, i in bids { if i > 0 { out.push(','); } emit_level(out, l); }
    out.append("],\"asks\":[");
    for l, i in asks { if i > 0 { out.push(','); } emit_level(out, l); }
    out.append("]}");
}
fn emit_report(out: u8[>..]&, m: Instrument, s: BookMeta, bids: Level[:], asks: Level[:],
    last: BookMeta, last_bids: Level[:], last_asks: Level[:],
    events: i64, markets: i64, resets: i64, history: u64) {
    format(out, "{\"schema\":1,\"engine\":\"goose\",\"mode\":\"verify_replay\",\"events\":", events,
        ",\"market_events\":", markets, ",\"resets\":", resets, ",\"price_decimals\":", m.pd,
        ",\"quantity_decimals\":", m.qd, ",\"checksum\":\"");
    hex64(out, book_hash(m, s, bids, asks)); out.append("\",\"history_checksum\":\"");
    hex64(out, history); out.append("\",\"current\":"); emit_state(out, m, s, bids, asks);
    out.append(",\"last_valid_before_reset\":");
    if last.valid { emit_state(out, m, last, last_bids, last_asks); } else { out.append("null"); }
    out.append("}");
}
