import book;

fn test_ok(ok: bool, checks: i64&) {
    if !ok { write_stderr(str("Goose kernel self-test failed at check ", checks + 1, "\n")); exit(1); }
    checks++;
}
fn test_rejected(m: Instrument, s: BookMeta&, bids: Level[>..<]&, asks: Level[>..<]&,
    last: BookMeta&, lb: Level[>..<]&, la: Level[>..<]&, e: Event, bc: Level[:], ac: Level[:], checks: i64&) {
    let before = s;
    let archived = last;
    var old_b = copy(bids);
    var old_a = copy(asks);
    var old_lb = copy(lb);
    var old_la = copy(la);
    test_ok(!apply_event(m, s, bids, asks, last, lb, la, e, bc, ac), checks);
    test_ok(s == before && last == archived && bids == old_b && asks == old_a && lb == old_lb && la == old_la, checks);
}
fn self_tests() -> i64 {
    var checks = 0;
    test_ok(hash_bytes(14695981039346656037, "hello") == 0xa430d84680aabd0b, checks);
    test_ok(crc32("123456789", false) == 0xcbf43926, checks);
    let m = Instrument { 1, 3, 5, 2 };
    var s = BookMeta {};
    var last = BookMeta {};
    var bids: Level[>..<] = [];
    var asks: Level[>..<] = [];
    var lb: Level[>..<] = [];
    var la: Level[>..<] = [];
    let reset = Event { 3,1,0,0,0,0,100,0,0,0,0 };
    test_ok(apply_event(m,s,bids,asks,last,lb,la,reset,[],[]), checks);
    var snap = Event { 1,1,2,10,12345,12344,101,201,0,2,2 };
    let initial_bids: Level[2] = [Level {1005,4}, Level {1000,2}];
    let initial_asks: Level[2] = [Level {1010,6}, Level {1020,8}];
    test_ok(apply_event(m,s,bids,asks,last,lb,la,snap,initial_bids,initial_asks), checks);
    test_ok(book_hash(m,s,bids,asks) == 0xe674268449ae430c, checks);
    let delta = Event { 2,1,3,900,12346,0,102,202,0,2,1 };
    let changes: Level[2] = [Level {1005,0}, Level {1000,10}];
    test_ok(apply_event(m,s,bids,asks,last,lb,la,delta,changes,[Level {1015,2}]), checks);
    test_ok(book_hash(m,s,bids,asks) == 0x3eee8e4070cae004, checks);
    test_ok(bids.len == 1 && bids[0] == Level {1000,10} && asks.len == 3, checks);
    test_ok(apply_event(m,s,bids,asks,last,lb,la,reset,[],[]), checks);
    test_ok(!s.valid && last.valid && lb.len == 1 && la.len == 3, checks);
    test_rejected(m,s,bids,asks,last,lb,la,delta,changes,[Level {1015,2}],checks);
    // Repopulate current state while retaining a nonempty archived state.
    snap.steady = 202;
    test_ok(apply_event(m,s,bids,asks,last,lb,la,snap,initial_bids,initial_asks), checks);
    var bad = delta;
    bad.nb = 1; bad.na = 1;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,2}],[Level {1015,-2}],checks);
    bad.steady = 1000000;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,2}],[Level {1015,-2}],checks);
    bad = delta; bad.nb = 1; bad.na = 0;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {996,2}],[],checks);
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,3}],[],checks);
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {0,2}],[],checks);
    bad.nb = 2;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,2},Level {995,4}],[],checks);
    bad.nb = 1; bad.session = 2;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,2}],[],checks);
    bad.session = 1; bad.u = 1;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,2}],[],checks);
    bad.u = 3; bad.kind = 255;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,2}],[],checks);
    bad.kind = 2; bad.steady = 200;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {995,2}],[],checks);
    bad = snap;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[Level {1000,2},Level {1005,2}],initial_asks,checks);
    bad = reset; bad.u = 1;
    test_rejected(m,s,bids,asks,last,lb,la,bad,[],[],checks);
    // Invalid future time did not advance the kernel clock.
    bad = delta; bad.nb = 0; bad.na = 0;
    test_ok(apply_event(m,s,bids,asks,last,lb,la,bad,[],[]), checks);
    // Dynamic growth: unlike the snapshot limit, there is no fixed book capacity.
    var many: Level[>..<] = [];
    for i in 4096 { many.push(Level {100000 - i*5,2}); }
    bad = delta; bad.u = 4; bad.nb = 4096; bad.na = 0;
    test_ok(apply_event(m,s,bids,asks,last,lb,la,bad,many,[]), checks);
    test_ok(bids.len == 4098, checks);
    bad.nb = 1; bad.u = 5;
    test_ok(apply_event(m,s,bids,asks,last,lb,la,bad,[Level {100005,2}],[]), checks);
    test_ok(bids.len == 4099, checks);
    bad.kind = 1; bad.u = 1; bad.nb = 0; bad.na = 2;
    test_ok(apply_event(m,s,bids,asks,last,lb,la,bad,[],initial_asks), checks);
    test_ok(bids.len == 0 && asks.len == 2, checks);
    // Positive int64 extremes make signed spread subtraction safe.
    let wide = Instrument {0,0,1,1};
    bad.nb = 1; bad.na = 1;
    test_ok(apply_event(wide,s,bids,asks,last,lb,la,bad,[Level {1,9223372036854775807}],[Level {9223372036854775807,1}]), checks);
    test_ok(asks[0].price - bids[0].price == 9223372036854775806, checks);
    checks
}
