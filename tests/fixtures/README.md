# Synthetic golden fixtures

These are artificial correctness fixtures, not a Bybit capture or performance measurements.
`instrument-synthetic.json` sets tickSize=0.50 and qtyStep=0.002: decimal places 1/3, integer steps 5/2.

Expected normalized events:

| Index | Kind | Session | u | seq | ts | cts | UTC | steady | BID/ASK |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | reset | 1 | 0 | 0 | 0 | 0 | 100 | 0 | 0/0 |
| 1 | snapshot | 1 | 2 | 10 | 12345 | 12344 | 101 | 201 | 2/2 |
| 2 | delta | 1 | 3 | 900 | 12346 | 0 | 102 | 202 | 2/1 |
| 3 | reset | 1 | 0 | 0 | 0 | 0 | 103 | 0 | 0/0 |
| 4 | reset | 2 | 0 | 0 | 0 | 0 | 104 | 0 | 0/0 |
| 5 | snapshot | 2 | 1 | 950 | 12347 | 0 | 105 | 203 | 0/2 |
| 6 | reset | 2 | 0 | 0 | 0 | 0 | 106 | 0 | 0/0 |

Integer price/quantity pairs, in payload order:
`(1005,4), (1000,2), (1010,6), (1020,8), (1005,0), (1000,10), (1015,2), (1010,2), (1020,4)`.

`events-v1-golden.hex` was encoded independently in Python using header format
`<8sIIIIqqQQQII16s8s32s`, record format `<IB3xQQQQQqqII`, level format `<qq`,
`zlib.crc32` and `hashlib.sha256`. Its size is `128 + 7*72 + 9*16 = 776` bytes.
Do not regenerate its expected bytes from the C++ writer under test.

`book-golden.json` contains manually specified post-event states and independently
computed Python FNV state/history checksums. See `docs/binary_format.md` for the byte specification.
