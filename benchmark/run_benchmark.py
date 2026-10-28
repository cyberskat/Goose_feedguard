"""Reproducible three-engine benchmark. Never manufactures missing results."""
import argparse
import csv
import datetime
import gzip
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess
import sys

from statistics_tools import quantiles
ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINES = ("cpp_map", "cpp_vector", "goose")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args, env):
    run = subprocess.run(list(map(str, args)), cwd=ROOT, env=env, capture_output=True,
                         text=True, encoding="utf-8", timeout=180)
    if run.returncode:
        raise ValueError(f"Process failed ({run.returncode}): {args[0]}\n{run.stderr}")
    return json.loads(run.stdout)


def cache_values(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def run(manifest, destination, runs):
    if runs < 10:
        raise ValueError("At least 10 measured runs per engine/mode/dataset are required")
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite {destination}")
    build = ROOT / "build/clang-benchmark"
    cache = cache_values(build / "CMakeCache.txt")
    if cache["CMAKE_BUILD_TYPE"] != "Release" or cache["CMAKE_CXX_FLAGS_RELEASE"].split() != ["-O2", "-DNDEBUG"]:
        raise ValueError("Build the aligned clang-benchmark preset (-O2 -DNDEBUG)")
    compiler = pathlib.Path(cache["CMAKE_CXX_COMPILER"])
    env = os.environ.copy()
    env["PATH"] = str(compiler.parent) + os.pathsep + env["PATH"]
    cpp, goose = build / "book_cpp.exe", ROOT / "goose/orderbook.exe"
    build_log = ROOT / "build/stage7-goose-build.log"
    log = build_log.read_text(encoding="utf-8-sig")
    if " -O2 " not in log or " -O0 " in log or "goose runtime 4d3da949e563" not in log:
        raise ValueError("Run scripts/build_benchmarks.ps1 to record the Release Goose build")
    versions = subprocess.run([str(compiler), "--version"], env=env, capture_output=True, check=True, text=True).stdout
    if "clang version 23.1.4" not in versions or "clang version 23.1.4" not in log:
        raise ValueError("Both backends must be the approved Clang 23.1.4")
    datasets = json.loads(manifest.read_text(encoding="utf-8"))
    if {d["name"] for d in datasets} != {"real", "synthetic"} or len(datasets) != 2:
        raise ValueError("Expected separately labelled real and synthetic inputs")
    sources = sorted(set([*ROOT.glob("include/*"), *ROOT.glob("src/**/*.cpp"), *ROOT.glob("src/**/*.c"),
                          *ROOT.glob("goose/*.goose"), *ROOT.glob("goose/*.build"),
                          *ROOT.glob("benchmark/*.py"), ROOT / "CMakeLists.txt", ROOT / "CMakePresets.json"]))
    source_hashes = {str(p.relative_to(ROOT)).replace("\\", "/"): sha(p) for p in sources if p.is_file()}
    hashes = {str(p): sha(p) for p in (cpp, goose, build_log)}
    data_hashes = {}
    verification = {}
    rows, details, latency_details, order = [], [], [], []

    def args(engine, dataset):
        base = [goose] if engine == "goose" else [cpp, "--engine", "map" if engine == "cpp_map" else "vector"]
        return base + ["--input", pathlib.Path(dataset["binary"])]

    for dataset in datasets:
        for key in ("binary", "capture", "metadata"):
            path = pathlib.Path(dataset[key])
            data_hashes[str(path)] = sha(path)
        if sha(pathlib.Path(dataset["binary"])) != dataset["sha256"]:
            raise ValueError("Manifest input SHA-256 mismatch")
        if sha(pathlib.Path(dataset["capture"])) != dataset["capture_sha256"]:
            raise ValueError("Manifest capture SHA-256 mismatch")
        reports = {engine: execute(args(engine, dataset), env) for engine in ENGINES}
        expected = {k: v for k, v in reports["cpp_map"].items() if k != "engine"}
        for report in reports.values():
            if {k: v for k, v in report.items() if k != "engine"} != expected:
                raise ValueError("Full state/history verification mismatch before timing")
        verification[dataset["name"]] = reports
        print(f"Verified {dataset['name']}: {expected['events']} records, history {expected['history_checksum']}", flush=True)

    def measured(engine, dataset, mode, sampled=False):
        row = execute(args(engine, dataset) + ["--measure", mode] + (["--latencies"] if sampled else []), env)
        expected = verification[dataset["name"]][engine]
        last = expected["last_valid_before_reset"]
        if (row["schema"] != 1 or row["engine"] != engine or row["mode"] != mode or
            row["events"] != expected["events"] or row["market_events"] != expected["market_events"] or
            row["checksum"] != expected["checksum"] or row["last_valid_checksum"] != (last["checksum"] if last else None) or
            row["elapsed_ns"] <= 0 or row["peak_working_set_bytes"] <= 0 or row["clock_frequency_hz"] <= 0 or
            row["latency_instrumented"] is not sampled or row["validation_in_timed_apply"] is not True or
            row["checksum_in_timed_loop"] is not False or
            len(row["latencies_ns"]) != (row["events"] if sampled else 0) or
            len(row["timer_samples_ns"]) != (10000 if sampled else 0)):
            raise ValueError("Measurement counts/checksums/metadata failed validation")
        return row

    # Warm every dataset, mode and engine. These invocations never enter results.csv.
    for dataset in datasets:
        for mode in ("core", "offline"):
            for engine in ENGINES:
                measured(engine, dataset, mode)
    frequencies = set()
    for di, dataset in enumerate(datasets):
        for run_number in range(1, runs + 1):
            modes = ("core", "offline") if run_number % 2 else ("offline", "core")
            for mi, mode in enumerate(modes):
                engine_order = [ENGINES[(run_number - 1 + di + mi + i) % 3] for i in range(3)]
                if run_number % 2 == 0:
                    engine_order.reverse()
                for engine in engine_order:
                    row = measured(engine, dataset, mode)
                    frequencies.add(row["clock_frequency_hz"])
                    key = {"dataset": dataset["name"], "mode": mode, "engine": engine, "run": run_number}
                    details.append({**key, **row})
                    order.append(key)
                    stats = {"p50_us": None, "p99_us": None, "p999_us": None,
                             "timer_p50_ns": None, "timer_p99_ns": None, "latency_samples": 0}
                    if mode == "core":
                        latency = measured(engine, dataset, mode, True)
                        frequencies.add(latency["clock_frequency_hz"])
                        latency_details.append(({**key, **latency}))
                        q, timer = quantiles(latency["latencies_ns"]), quantiles(latency["timer_samples_ns"])
                        stats = {"p50_us": q["p50"]/1000, "p99_us": q["p99"]/1000, "p999_us": q["p999"]/1000,
                                 "timer_p50_ns": timer["p50"], "timer_p99_ns": timer["p99"],
                                 "latency_samples": len(latency["latencies_ns"])}
                    rows.append({**key, "events": row["events"], "market_events": row["market_events"],
                                 "elapsed_ns": row["elapsed_ns"], "events_per_sec": row["events"]*1e9/row["elapsed_ns"],
                                 **stats, "peak_memory_mb": row["peak_working_set_bytes"]/1e6,
                                 "clock_resolution_ns": 1e9/row["clock_frequency_hz"],
                                 "checksum": row["checksum"], "last_valid_checksum": row["last_valid_checksum"]})
            print(f"Measured {dataset['name']} round {run_number}/{runs}", flush=True)
    if len(frequencies) != 1:
        raise ValueError("Inconsistent QPC frequencies across programs/runs")
    for path, digest in {**hashes, **data_hashes}.items():
        if sha(pathlib.Path(path)) != digest:
            raise ValueError(f"Artifact changed during measurements: {path}")
    for path, digest in source_hashes.items():
        if sha(ROOT / path) != digest:
            raise ValueError(f"Source changed during measurements: {path}")
    cpu = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                          "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name"],
                         capture_output=True, text=True, encoding="utf-8", check=True).stdout.strip()
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True)
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True)
    environment = {"created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "cpu": cpu,
                   "platform": platform.platform(), "python": sys.version, "compiler_version": versions.strip(),
                   "cpp_flags": cache["CMAKE_CXX_FLAGS_RELEASE"], "native_optimization": "-O2",
                   "goose_build_log": log, "build_log_sha256": sha(build_log),
                   "compile_commands": json.loads((build/"compile_commands.json").read_text()),
                   "binary_and_log_hashes": hashes, "input_hashes": data_hashes, "source_hashes": source_hashes,
                   "datasets": datasets, "verification_replays": verification, "run_order": order,
                   "git_commit": commit.stdout.strip() if commit.returncode == 0 else None,
                   "working_tree_dirty": bool(dirty.stdout.strip()), "measured_runs": runs,
                   "warmup_runs_per_engine_mode_dataset": 1, "clock_frequency_hz": frequencies.pop(),
                   "clock": "shared native Windows QPC, integer nanoseconds",
                   "memory": "whole-process PeakWorkingSetSize, decimal MB; throughput process only",
                   "latency_scope": "core apply including validation/reset, separate instrumented process",
                   "quantiles": "nearest rank, per run; raw samples include timer overhead, no subtraction",
                   "offline_percentiles": None, "offline_cache": "warm OS file cache after warmup, not cold-disk bandwidth",
                   "repeat_per_process": 1, "event_denominator": "all records including resets",
                   "affinity_and_power_policy": "not changed", "background_processes": "not controlled"}
    destination.mkdir(parents=True, exist_ok=False)
    (destination/"latencies").mkdir()
    with (destination/"results.csv").open("x", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    for filename, value in (("environment.json", environment), ("raw_runs.json", details)):
        with (destination/filename).open("x", encoding="utf-8") as output:
            json.dump(value, output, indent=2); output.write("\n")
    for value in latency_details:
        name = f"{value['dataset']}-{value['engine']}-{value['run']:02d}.json.gz"
        with gzip.open(destination/"latencies"/name, "wt", encoding="utf-8") as output:
            json.dump(value, output)
    summary = ["# Three-engine benchmark", "", f"CPU: {cpu}. Clang 23.1.4; native -O2.", "",
               "Ten or more measured runs per group, one full-file replay per process. Medians of per-run metrics.", "",
               "| Dataset | Mode | Engine | events/sec | p50 us | p99 us | p99.9 us | Max peak MB |",
               "|---|---|---|---:|---:|---:|---:|---:|"]
    for dataset in datasets:
        for mode in ("core", "offline"):
            for engine in ENGINES:
                group = [r for r in rows if r["dataset"] == dataset["name"] and r["mode"] == mode and r["engine"] == engine]
                def median(key):
                    values = [r[key] for r in group if r[key] is not None]
                    return f"{statistics.median(values):.3f}" if values else "n/a"
                summary.append(f"| {dataset['name']} | {mode} | {engine} | {median('events_per_sec')} | {median('p50_us')} | {median('p99_us')} | {median('p999_us')} | {max(r['peak_memory_mb'] for r in group):.3f} |")
    summary += ["", "Throughput excludes per-event clocks; latency comes from separate instrumented runs. Offline percentiles are intentionally unavailable; offline measures the entire file pipeline. Peak RAM includes loading/preparation and is measured in uninstrumented processes.", "",
                "Timer calibration, raw samples, all commands, hashes and replay checks are saved. No automatic timer-overhead subtraction. Results are local; CPU frequency, affinity and background applications were not controlled.", ""]
    (destination/"benchmark_summary.md").write_text("\n".join(summary), encoding="utf-8")
    print("\n".join(summary))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=pathlib.Path, default=ROOT/"data/benchmark-stage7/datasets.json")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT/"results/stage7-20261028")
    parser.add_argument("--runs", type=int, default=10)
    args = parser.parse_args()
    try:
        run(args.manifest.resolve(), args.output.resolve(), args.runs)
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Benchmark failed: {error}\n")
