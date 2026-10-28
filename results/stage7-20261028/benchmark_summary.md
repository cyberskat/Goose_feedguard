# Three-engine benchmark

CPU: 12th Gen Intel(R) Core(TM) i7-12650H. Clang 23.1.4; native -O2.

Ten or more measured runs per group, one full-file replay per process. Medians of per-run metrics.

| Dataset | Mode | Engine | events/sec | p50 us | p99 us | p99.9 us | Max peak MB |
|---|---|---|---:|---:|---:|---:|---:|
| real | core | cpp_map | 1981440.124 | 0.300 | 3.750 | 8.950 | 8.643 |
| real | core | cpp_vector | 2389914.323 | 0.300 | 2.900 | 6.200 | 8.630 |
| real | core | goose | 3831108.412 | 0.200 | 1.800 | 4.250 | 7.664 |
| real | offline | cpp_map | 983171.123 | n/a | n/a | n/a | 8.643 |
| real | offline | cpp_vector | 1076818.856 | n/a | n/a | n/a | 8.634 |
| real | offline | goose | 1381320.982 | n/a | n/a | n/a | 7.664 |
| synthetic | core | cpp_map | 14089607.493 | 0.100 | 0.200 | 0.600 | 26.751 |
| synthetic | core | cpp_vector | 19675546.295 | 0.100 | 0.100 | 0.250 | 26.743 |
| synthetic | core | goose | 36215040.099 | 0.000 | 0.100 | 0.100 | 25.747 |
| synthetic | offline | cpp_map | 2698074.550 | n/a | n/a | n/a | 26.747 |
| synthetic | offline | cpp_vector | 2839549.571 | n/a | n/a | n/a | 26.751 |
| synthetic | offline | goose | 3438153.713 | n/a | n/a | n/a | 25.752 |

Throughput excludes per-event clocks; latency comes from separate instrumented runs. Offline percentiles are intentionally unavailable; offline measures the entire file pipeline. Peak RAM includes loading/preparation and is measured in uninstrumented processes.

Timer calibration, raw samples, all commands, hashes and replay checks are saved. No automatic timer-overhead subtraction. Results are local; CPU frequency, affinity and background applications were not controlled.

QPC frequency is 10 MHz: nominal clock quantum is 100 ns (0.1 us). A synthetic p50 of 0.000 us means samples fell below that quantum, not zero execution time. Synthetic p50 values at or below 0.1 us must not be interpreted as precise sub-100-ns timings. The saved CSV retains unmodified raw quantiles; audit.json flags this limitation.
