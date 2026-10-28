# Binary input and canonical checksums

All integers are little-endian. Signed i64 values use two's complement.
The file is serialized field-by-field, never as an in-memory C++ structure.
The supported instrument is `linear / BTCUSDT / orderbook.50.BTCUSDT`.

## Header: 128 bytes

| Offset | Type | Meaning |
|---|---|---|
| 0 | 8 bytes | ASCII `BFGDEVT1` |
| 8 | u32 | Version: 1 |
| 12 | u32 | Header size: 128 |
| 16 | u32 | Price decimal places: 0–18 |
| 20 | u32 | Quantity decimal places: 0–18 |
| 24 | i64 | Positive integer tick size |
| 32 | i64 | Positive integer quantity step |
| 40 | u64 | Total records, including resets |
| 48 | u64 | Market records: snapshots and deltas |
| 56 | u64 | Payload byte count |
| 64 | u32 | Payload CRC32 |
| 68 | u32 | Header CRC32, treating bytes 68–71 as zero |
| 72 | 16 bytes | `BTCUSDT`, zero-padded |
| 88 | 8 bytes | `linear`, zero-padded |
| 96 | 32 bytes | SHA-256 of the exact saved instrument-metadata bytes |

CRC32 is IEEE/ISO-HDLC, reflected polynomial `0xedb88320`, initial and final
XOR `0xffffffff`. The check vector `123456789` produces `cbf43926`.
Checksums detect accidental corruption; the metadata hash links an input file
to a saved response, rather than authenticating its publisher.

## Record: 72-byte header plus levels

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | Total record bytes |
| 4 | u8 | Snapshot=1, delta=2, reset=3 |
| 5 | 3 bytes | Reserved; zero |
| 8 | u64 | Session, positive |
| 16 | u64 | Exchange update ID u |
| 24 | u64 | Cross-sequence seq |
| 32 | u64 | Exchange publication ts, milliseconds |
| 40 | u64 | Matching-engine cts, milliseconds; zero if absent |
| 48 | i64 | Local receive UTC, Unix nanoseconds |
| 56 | i64 | Local receive steady clock, nanoseconds |
| 64 | u32 | BID change count |
| 68 | u32 | ASK change count |
| 72 | 16 bytes per level | All BID changes, then all ASK changes |

A level is price i64 followed by quantity i64. Record length is exactly
`72 + 16 * (bid_count + ask_count)`. No trailing bytes are allowed.
The current reader limits the file to 1 GiB and each side's changes to 4096.
Goose's whole-file loader checks the file-size limit after loading it.

Prices are positive; quantities are nonnegative and multiples of their steps.
A zero quantity deletes a delta level and is forbidden in a snapshot.
Duplicate prices within one side/event are rejected. Snapshot depth is at most
50 per side, BID strictly descending and ASK strictly ascending. Empty sides
are supported. Delta ordering is retained; the book is not silently truncated.

## Exact decimal conversion

`integer = original decimal * 10^decimal_places` without double. Decimal places
come from tickSize/qtyStep after removing insignificant fractional zeros.
For tickSize `0.10`, price places=1 and integer tick=1. For qtyStep `0.001`,
quantity places=3 and integer step=1. A tickSize of `0.50` gives places=1, step=5.

Extra fractional zeros are allowed; losing a nonzero digit is rejected.
Exponent notation, NaN, whitespace, leading/trailing decimal points and `+`
are rejected. The shared decimal parser checks signed int64 boundaries;
market prices and quantities additionally reject negative values.

## Session semantics

Capture session_start and disconnect records become resets. A reset has only
session and positive receive UTC; other record fields and counts are zero.
Session cannot decrease. Market data belongs to the current session; deltas
require its snapshot. u=1 is accepted only for a snapshot. seq need not advance
by one. Market steady timestamps do not decrease across the file; UTC can adjust.

The terminal reset invalidates current quotes. Engines separately preserve
the last trustworthy state and the history checksum.

## State checksum: FNV-1a-64

Offset basis `14695981039346656037`; prime `1099511628211`.
For each byte: `h = (h XOR byte) * prime modulo 2^64`.

Canonical bytes:

1. Eight ASCII bytes `BFGBOOK1`.
2. Price places u32, quantity places u32.
3. Tick size i64, quantity step i64.
4. Valid flag u8, 0 or 1.
5. Session u64, u u64, seq u64.
6. BID count u64, ASK count u64.
7. BID descending, then ASK ascending: price i64 and quantity i64 per level.

All numbers use little-endian encoding. The fixed state prefix is 73 bytes.
Receive timestamps and the full metadata-response hash are excluded from this
state checksum. Scales and instrument steps are included.

History starts with nine ASCII bytes `BFGTRACE1`. After each successful event,
append kind u8, session u64, u u64, seq u64, and the post-event state checksum
u64 to the running FNV state. This binds intermediate states even if reset
later empties the book. FNV is a consistency checksum, not a security primitive.

The synthetic golden ledger is in `tests/fixtures/book-golden.json`. Its binary
fixture contains 776 independently encoded bytes in `events-v1-golden.hex`.
