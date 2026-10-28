# Reproduce FeedGuard

## Inspect the saved experiment first

Extract the review bundle or check out the repository, then work from its root.
Python 3.12.10 was used for the recorded run. Evidence-only commands use the
standard library and do not require compiling native programs.

```powershell
python benchmark/prepare_datasets.py
python benchmark/audit_results.py results/stage7-20261028 --portable --output build/shipped-evidence-audit.json
python scripts/validate_capture.py data/raw/btcusdt-20261027-linear-x25519.jsonl --expected 10000
python scripts/verify_normalized.py data/normalized/events.bin data/raw/btcusdt-20261027-linear-x25519.jsonl data/metadata/btcusdt-linear-20261027.json
```

The first command creates the synthetic workload used by the saved experiment.
Its binary and JSONL hashes must match the evidence. Run it once on a clean
checkout; it refuses to overwrite an existing dataset directory.

Portable audit checks the saved numerical results, raw samples, input hashes,
build log and measured source snapshot. It reports `recorded_hashes_only` for
the original executables: those binaries are not required for inspecting the
evidence, and a local rebuild is not a bit-for-bit reproduction of their PE files.

The archive also contains `RELEASE_MANIFEST.json`, covering every packaged file
except the manifest itself. Review the [technical report](technical_report.md)
before interpreting the ratios.

## Toolchains

Native programs and process-memory measurements target Windows x64.
The tested environment used:

| Component | Tested version |
|---|---|
| GCC / MINGW64 | 16.2.0 |
| Clang / CLANG64 | 23.1.4 |
| CMake | 4.4.4; project minimum 3.27 |
| Ninja | 1.13.2 |
| Boost | 1.92.0 |
| OpenSSL | 3.6.5 |
| simdjson | 5.0.5 |
| Catch2 | 3.16.1 |
| Goose | v2026.3, runtime `4d3da949e563` |

Install [MSYS2](https://www.msys2.org/) in `C:\msys64` and complete its normal
system update. In an MSYS2 shell, install the CLANG64 packages:

```sh
pacman -Syu
pacman -S --needed mingw-w64-clang-x86_64-toolchain mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-boost mingw-w64-clang-x86_64-openssl mingw-w64-clang-x86_64-simdjson mingw-w64-clang-x86_64-catch
```

For the additional GCC profiles, install the corresponding MINGW64 family:

```sh
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-boost mingw-w64-x86_64-openssl mingw-w64-x86_64-simdjson mingw-w64-x86_64-catch
```

Do not mix libraries from MINGW64, CLANG64 or CLion's bundled MinGW. The project
presets isolate their PATH, compiler and package prefix. MSYS2 is a rolling
distribution; retain the environment metadata when running with newer packages.

Download the Windows x64 archive for
[Goose v2026.3](https://github.com/aardappel/goose/releases/tag/v2026.3) and unpack it
under `C:\tools\goose\goose-v2026.3-windows-x64`. The supplied build script accepts
`-Goose path\goose.exe` and `-Clang path\clang.exe` if installed elsewhere.

## Build the aligned measurement programs

From PowerShell:

```powershell
.\scripts\build_benchmarks.ps1
$env:PATH = 'C:\msys64\clang64\bin;' + $env:PATH
.\build\clang-benchmark\book_cpp.exe --input data/normalized/events.bin --engine map
.\build\clang-benchmark\book_cpp.exe --input data/normalized/events.bin --engine vector
.\goose\orderbook.exe --input data/normalized/events.bin
```

The builder compiles the C++ benchmark preset at `-O2 -DNDEBUG`, runs CTest,
builds Goose/native C at `-O2`, runs its kernel assertions, and records the Goose
build log. It keeps all artifacts under `build/` and `goose/orderbook.exe`.

Expected real replay values:

```text
events:                  10002
market_events:           10000
checksum:                e898d48c8118d4d9
history_checksum:        133ad3f85b1d7b87
last_valid checksum:     a8bea05ec3dc51c7
```

The current book is invalid/empty after the terminal disconnect/reset; the last
valid book contains 50 bids and 50 asks. Compare the archived state and history,
rather than only the final empty state.

## Run the full correctness checks

The Python integration checks also use the normal Clang Release executable
and require Debug programs for boundary and measurement-mode checks:

```powershell
$env:PATH = 'C:\msys64\clang64\bin;' + $env:PATH
cmake --preset clang-release
cmake --build --preset clang-release --target book_cpp unit_tests --parallel 2
ctest --preset clang-release
cmake --preset clang-debug
cmake --build --preset clang-debug --target book_cpp unit_tests --parallel 2
ctest --preset clang-debug
.\scripts\build_goose.ps1
python tests/test_goose.py
python tests/test_goose_boundaries.py
python tests/test_benchmark.py
```

The additional `gcc-release` and `gcc-debug` presets follow the same CMake/CTest
pattern with `C:\msys64\mingw64\bin` first on PATH. Use a fresh shell when
switching toolchain families.

## Run another measured experiment

Stop other compilation/test jobs before measuring. Record any additional
controls you apply to power policy, background load or core affinity.

```powershell
.\scripts\build_benchmarks.ps1
python benchmark/run_benchmark.py --runs 10 --output results/my-new-run
python benchmark/audit_results.py results/my-new-run --freeze-sources
```

The output directory must be new. If the synthetic dataset was generated in a
different directory, pass its `datasets.json` with `--manifest`.
The saved runner currently requires the pinned Clang 23.1.4 and aligned flags.
Record a new environment when changing the compiler or measurement protocol.

Each group has a warmup and at least ten measured runs. The runner rotates
engine order, separates throughput from instrumented latency, verifies each
result and rejects changed inputs/binaries during the experiment. Offline
percentiles remain empty; its throughput includes reading and decoding.

## Regenerate the figures

Plot dependencies are isolated from the native project:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r benchmark/requirements-plots.txt
.\.venv\Scripts\python.exe benchmark/plot_results.py --results results/stage7-20261028 --output build/regenerated-figures
```

The output directory must be new. The script reads existing CSV only, requires
at least ten valid runs for each engine, and writes two 1600x900 PNGs plus a
manifest binding them to the input CSV/environment hashes. Missing results
produce an error, not sample bars. Whiskers are observed min–max ranges.

## Capture and normalize a fresh feed

```powershell
$env:PATH = 'C:\msys64\clang64\bin;' + $env:PATH
cmake --build --preset clang-benchmark --target collector converter --parallel 2
python scripts/fetch_instrument.py data/metadata/btcusdt-fresh.json
.\build\clang-benchmark\collector.exe --count 10000 --output data/raw/btcusdt-fresh.jsonl --timeout 600
python scripts/validate_capture.py data/raw/btcusdt-fresh.jsonl --expected 10000
.\build\clang-benchmark\converter.exe --input data/raw/btcusdt-fresh.jsonl --instrument data/metadata/btcusdt-fresh.json --output data/normalized/events-fresh.bin
```

Use new filenames. Capture uses the public Bybit linear endpoint and verified
TLS, without credentials. Save metadata before a new capture; the historical
included capture used metadata fetched afterward, with grid compatibility checked.
Ctrl+C and timeout write a final journal record. A close-handshake timeout is
retained as an explicit disconnect reason.

## Package a review snapshot

```powershell
python scripts/prepare_github.py --output build/github-review
```

The exporter selects English-facing documents, code, recorded input and the
measured evidence. It excludes local environments, generated executables,
temporary runs, unrelated captures and internal staged notes. The synthetic
workload is regenerated by the supplied script. It writes a ZIP and per-file
SHA-256 manifest. Publication inputs are listed in [publication.md](publication.md).
