# Bybit FeedGuard: C++ and Goose order-book processing

**Review snapshot — 28 October 2026**

## Executive result

FeedGuard is a working Windows x64 prototype that records Bybit market messages,
normalizes them without floating-point conversion, and replays the same binary
events through three independently implemented order-book engines.

On an Intel Core i7-12650H with Clang 23.1.4 and native `-O2`, the recorded
BTCUSDT depth-50 workload produced these local observations:

| Engine | Median core events/sec | Median per-run p99 | Maximum process peak RAM |
|---|---:|---:|---:|
| C++ map | 1,981,440 | 3.75 us | 8.643 MB |
| Optimized C++ vector | 2,389,914 | 2.90 us | 8.630 MB |
| Goose | 3,831,108 | 1.80 us | 7.664 MB |

The primary comparison is Goose against optimized C++: **1.603x median core
throughput** and **37.9% lower median per-run p99** in the instrumented experiment.
The offline throughput ratio was **1.283x**. These are different measurements
and should not be substituted for one another.

![Core throughput](../results/stage7-20261028/figures/throughput.png)

![p99 apply latency](../results/stage7-20261028/figures/latency_p99.png)

## System and data

```text
Bybit public WebSocket
    -> collector -> raw JSONL
    -> converter + saved instrument metadata -> events.bin
    -> C++ map / C++ vector / Goose
    -> state verification -> timed runs -> CSV and raw samples
```

The collector uses asynchronous Boost.Beast, verified TLS, SNI, JSON heartbeat,
bounded reconnect backoff, session identifiers and local receive timestamps.
The converter uses simdjson and checked decimal-to-int64 conversion.

The real input is a recorded `orderbook.50.BTCUSDT` stream from Bybit's public
linear market: **10,000 market messages and two explicit reset records**.
It represents the subscribed depth, rather than the entire exchange order book.
The binary contains 62,023 level changes. Prices use scale 10 and quantities
scale 1000, derived from saved tickSize/qtyStep metadata.

Metadata was fetched after the capture. All recorded values were checked for
compatibility with its steps; this does not establish that the specification
was historically unchanged throughout the capture.

The separate synthetic input has **100,100 records**, including 99,900 market
events and 200 resets across 100 sessions. It exercises repeated snapshots,
updates, deletions, empty sides and sequence gaps. It is labelled as artificial
throughout the results.

## Implementations

**C++ map:** descending BID and ascending ASK maps. Incoming changes are checked
before modification; new nodes for both sides are prepared before commit.
The entire book is not copied for each delta.

**C++ vector:** contiguous sorted levels, binary search and range shifts.
The initial capacity is 64 per side; capacity grows when required. Final sizes
are calculated before commit, and deletions precede insertions. It uses the
same event-validation function as the map implementation.

**Goose v2026.3:** its own binary reader, CRC32, validation, sorted-array kernel,
state archive, FNV and JSON output. It compiles to C through the language's
normal backend. It does not call either C++ order-book implementation.
Growable arrays remain separate because this Goose release allows only one
resizable tail field in a structure.

The shared native C helper contains only Windows QPC and memory counters.
It does not implement parsing, checksums or order-book operations.

## Correctness evidence

- All engines consume the same versioned little-endian binary format.
- CRC32, lengths, scales, level ranges, sessions, snapshot order and decimal
  precision are checked.
- A reset invalidates the current book and preserves the last trustworthy state.
- Canonical FNV-1a-64 hashes bind scales, steps, state validity, identifiers,
  counts and sorted levels. A history checksum covers every post-event state.
- Checking the last-valid state and history avoids a vacuous comparison of empty
  books after the final reset.
- 38 C++ tests passed in the supported GCC/Clang Debug/Release profiles and in
  the aligned Clang benchmark profile.
- Goose Release and Debug each passed 46 built-in kernel assertions.
- Independent Python replay compared every state on golden, real and synthetic
  inputs in both Goose builds: 220,218 state comparisons in total.
- The corruption suite recorded 3,163 rejection checks, including C++ controls.
  Additional valid boundary cases covered int64.max, uint64.max, reset-only
  input and an empty but valid snapshot.

Real replay checksums:

| Check | Value |
|---|---|
| Current state after final reset | `e898d48c8118d4d9` |
| Last valid state | `a8bea05ec3dc51c7` |
| Entire state history | `133ad3f85b1d7b87` |

FNV and CRC serve consistency/error detection. They are not cryptographic
authentication mechanisms.

## Experiment design

All native backends use Clang 23.1.4 with `-O2`; C++ additionally defines
`NDEBUG`. Explicit event checks remain enabled. The C++ and Goose runtime
representations are language-specific and are included in what is measured.

Each dataset/mode/engine combination receives one warmup and ten measured runs.
Engine order rotates and reverses; core/offline order alternates. Each process
replays the file once with a fresh book. The results comprise **120 throughput
runs and 60 separate latency runs**.

### Core throughput

File reading, decoding and book construction happen before the timer. The timed
region contains apply, including event validation, snapshot/reset work and
last-valid archival. Checksums and result serialization happen afterward.

### Offline throughput

The timed region starts before reading the file and includes CRC validation,
decoding, book construction and replay. Checksums, JSON and book destruction
are outside it. Warmup leaves the OS file cache warm; this is not a cold-disk
bandwidth experiment.

### Latency

Per-event timestamps are collected in separate core processes, so they do not
distort the throughput measurements. Percentiles use nearest rank
`ceil(q * N)`, with one-based ranks. The reported table uses medians of ten
per-run percentiles. It does not use percentiles of average batch times.

Each latency process also records 10,000 empty timer brackets. Overhead is
reported and is not subtracted from percentiles. Offline percentile cells are
empty because apply-only timings would not represent the full file pipeline.

### Memory

Memory is Windows `PeakWorkingSetSize / 1,000,000`, measured in each
uninstrumented throughput process. It includes loading and preparation, rather
than just book storage. The table reports the largest observed peak across ten
runs. Reserved virtual address space is not counted as committed physical RAM.

## Complete results

All values below are medians of per-run metrics, except maximum peak RAM.

| Recorded input, core | events/sec | p50 us | p99 us | p99.9 us | Max peak MB |
|---|---:|---:|---:|---:|---:|
| C++ map | 1,981,440 | 0.30 | 3.75 | 8.95 | 8.643 |
| C++ vector | 2,389,914 | 0.30 | 2.90 | 6.20 | 8.630 |
| Goose | 3,831,108 | 0.20 | 1.80 | 4.25 | 7.664 |

| Input | Mode | C++ map events/sec | C++ vector events/sec | Goose events/sec |
|---|---|---:|---:|---:|
| Recorded | Offline | 983,171 | 1,076,819 | 1,381,321 |
| Synthetic | Core | 14,089,607 | 19,675,546 | 36,215,040 |
| Synthetic | Offline | 2,698,075 | 2,839,550 | 3,438,154 |

The recorded core throughput ranges across ten runs were 1.887–2.038 million/s
for map, 2.280–2.511 million/s for vector, and 3.533–3.920 million/s for Goose.
Chart whiskers show observed min–max ranges, not confidence intervals.

## Measurement limits

The shared QPC frequency was 10 MHz: a **100 ns quantum**. Empty timer brackets
had a median of 0 ns and p99 of 100 ns. Zero means both timestamps fell in the
same quantum, not that the timer or operation was free.

Synthetic Goose p50 is zero in the raw CSV. Synthetic p50 values for all engines
are at or below timer resolution. They cannot support precise sub-100-ns
latency comparisons; batch throughput is the useful measurement for that case.
Raw samples were retained without correction or replacement.

The experiment uses one mobile Intel CPU and one recorded instrument/depth.
Background applications, CPU frequency and core affinity were not controlled.
The report provides no confidence interval or universal language ranking.
Tail latency includes measurement overhead and normal OS scheduling effects.

## Separate live test

The fresh live test captured 2,000 messages over an observed market span of
63.694 seconds: one snapshot, 1,999 deltas, three pong responses and no reconnects.
During shutdown, the WebSocket close acknowledgement exceeded the three-second
deadline. The collector recorded `close timeout`, closed the socket and flushed
the validated journal. That shutdown outcome is retained in the evidence.

The live test measures connection behavior. Exchange-to-local latency was not
measured, and it does not establish a language-speed comparison.

## Reproduction and provenance

The evidence includes the CSV, native responses, gzip latency distributions,
timer samples, compiler commands, original build log, input hashes and a
byte-preserving snapshot of the measured source files. Audit checked 120 rows
and 3,303,060 latency samples against those raw records.

The original working tree had no commit; environment metadata records
`git_commit=null` and `working_tree_dirty=true`. Original absolute build paths
remain in the provenance record. Portable audit relocates dataset paths and
checks shipped evidence; the old executable hashes remain historical identifiers.
Locally rebuilt executables are not claimed to be byte-identical.

| Artifact | SHA-256 |
|---|---|
| Recorded JSONL | `f8bec2387391e0d9e41c5f9bb125bec5022f8ef2697a94cefa6de3571a21c207` |
| Normalized binary | `58062375a318a015a7032648d978af9edcdc254432a61bead4899612ba2ebe41` |

See [reproduction commands](reproduction.md), [binary/checksum specification](binary_format.md),
[measured CSV](../results/stage7-20261028/results.csv) and the
[pilot proposal draft](pilot_proposal.md).
