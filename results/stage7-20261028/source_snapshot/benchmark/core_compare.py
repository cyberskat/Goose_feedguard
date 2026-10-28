"""Preliminary map/vector core-only comparison; no Goose or latency claims."""
import argparse
import csv
import datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args, env=None):
    return subprocess.run(args, cwd=ROOT, env=env, check=True, capture_output=True, text=True, encoding="utf-8")


def compare(build_dir, source, destination, runs=10, repeats=50):
    if runs < 10 or not 1 <= repeats <= 1000:
        raise ValueError("At least 10 runs and repeat in 1..1000 are required")
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite {destination}")
    cache = {}
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
    if cache.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("A Release build is required")
    compiler = pathlib.Path(cache["CMAKE_CXX_COMPILER"])
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment["PATH"]
    version = command([str(compiler), "--version"], environment).stdout.strip()
    if "clang" not in version.lower():
        raise ValueError("Use the approved Clang build for this preliminary comparison")
    executable = build_dir / ("book_cpp.exe" if os.name == "nt" else "book_cpp")

    def invoke(engine, benchmark=False):
        args = [str(executable), "--input", str(source), "--engine", engine]
        if benchmark:
            args += ["--benchmark", "--repeat", str(repeats)]
        return json.loads(command(args, environment).stdout)

    map_report, vector_report = invoke("map"), invoke("vector")
    if {k: v for k, v in map_report.items() if k != "engine"} != {k: v for k, v in vector_report.items() if k != "engine"}:
        raise ValueError("Verification replay results differ")
    reference = (map_report["checksum"], map_report["history_checksum"],
                 map_report["last_valid_before_reset"]["checksum"] if map_report["last_valid_before_reset"] else None)

    def checked(engine):
        row = invoke(engine, True)
        if (row["checksum"], row["verified_history_checksum"], row["last_valid_checksum"]) != reference:
            raise ValueError("Benchmark checksum mismatch")
        if (row["source_events"] != map_report["events"] or row["repeat"] != repeats or
                row["events"] != map_report["events"] * repeats or row["elapsed_ns"] <= 0 or
                row["source_market_events"] != map_report["market_events"] or
                row["checksum_in_timed_loop"] is not False or row["validation_in_timed_apply"] is not True):
            raise ValueError("Invalid benchmark measurement metadata")
        return row

    # One full process warmup per engine; excluded from the CSV.
    checked("map")
    checked("vector")
    rows, raw_runs, orders = [], [], []
    for run in range(1, runs + 1):
        order = ["map", "vector"] if run % 2 else ["vector", "map"]
        orders.append(order)
        for engine in order:
            measured = checked(engine)
            raw_runs.append({"run": run, **measured})
            rows.append({"engine": measured["engine"], "run": run, "events": measured["events"],
                         "elapsed_ns": measured["elapsed_ns"],
                         "events_per_sec": measured["events"] * 1e9 / measured["elapsed_ns"],
                         "ns_per_event": measured["elapsed_ns"] / measured["events"],
                         "checksum": measured["checksum"], "last_valid_checksum": measured["last_valid_checksum"],
                         "verified_history_checksum": measured["verified_history_checksum"]})
        print(f"Completed pair {run}/{runs}: {', '.join(order)}", flush=True)

    cpu = platform.processor()
    if os.name == "nt":
        try:
            cpu = command(["powershell.exe", "-NoProfile", "-Command",
                           "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name"]).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True)
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True)
    compile_database = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    sources = sorted(list((ROOT / "include").glob("*.hpp")) + list((ROOT / "src").rglob("*.cpp")) +
                     [ROOT / "CMakeLists.txt", ROOT / "CMakePresets.json", pathlib.Path(__file__).resolve()])
    source_hashes = {str(path.relative_to(ROOT)).replace("\\", "/"): digest(path) for path in sources}
    metadata = {"created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "cpu": cpu,
                "platform": platform.platform(), "python": sys.version, "compiler_version": version,
                "compiler": str(compiler), "build_dir": str(build_dir),
                "cxx_flags": cache.get("CMAKE_CXX_FLAGS"), "release_flags": cache.get("CMAKE_CXX_FLAGS_RELEASE"),
                "compile_commands": [entry for entry in compile_database if pathlib.Path(entry["file"]).name in
                                     ("book_map.cpp", "book_vector.cpp", "book_validation.cpp", "core_benchmark.cpp")],
                "input": str(source), "input_sha256": digest(source), "binary_sha256": digest(executable),
                "source_hashes": source_hashes, "git_commit": commit.stdout.strip() if commit.returncode == 0 else None,
                "working_tree_dirty": bool(dirty.stdout.strip()) if dirty.returncode == 0 else None,
                "mode": "core_only_preliminary", "measured_runs_per_engine": runs, "warmup_processes_per_engine": 1,
                "repeat": repeats, "source_events": map_report["events"], "source_market_events": map_report["market_events"],
                "run_order": orders, "event_denominator": "all records including resets",
                "repeated_sequence": True, "validation_in_timed_apply": True,
                "checksum_in_timed_loop": False, "book_construction_in_timed_loop": False,
                "book_destruction_in_timed_loop": False,
                "timing_note": "Fresh preconstructed book for each repetition; full per-event engine comparison before timing; post-run current and archived states checked for every repetition.",
                "verification_replays": {"cpp_map": map_report, "cpp_vector": vector_report}}
    medians = {engine: statistics.median(r["events_per_sec"] for r in rows if r["engine"] == engine)
               for engine in ("cpp_map", "cpp_vector")}
    speedup = medians["cpp_vector"] / medians["cpp_map"]
    destination.mkdir(parents=True, exist_ok=False)
    with (destination / "core_results.csv").open("x", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for name, value in (("environment.json", metadata), ("raw_runs.json", raw_runs)):
        with (destination / name).open("x", encoding="utf-8") as output:
            json.dump(value, output, indent=2)
            output.write("\n")
    summary = (f"# Preliminary C++ core-only comparison\n\nCPU: {cpu}.\n\n"
               f"Compiler: {version.splitlines()[0]}; Release flags: {cache.get('CMAKE_CXX_FLAGS_RELEASE')}.\n\n"
               f"{runs} measured runs per engine after one warmup; {repeats} repetitions of {map_report['events']} source records per run.\n\n"
               "| Engine | Median events/sec | Median ns/event |\n|---|---:|---:|\n" +
               "".join(f"| {engine} | {medians[engine]:,.0f} | {statistics.median(r['ns_per_event'] for r in rows if r['engine'] == engine):.2f} |\n"
                       for engine in ("cpp_map", "cpp_vector")) +
               f"\nVector/map ratio of median throughput: {speedup:.3f}x.\n\n"
               "The same recorded sequence is repeated; this is not a larger set of unique market events. Validation and reset archival are timed. File I/O, book preparation/destruction, full history checks and post-run verification are untimed.\n\n"
               "These are local preliminary measurements; Goose, p99 latency and peak RAM are not measured here. The full three-engine benchmark is a later stage.\n")
    (destination / "summary.md").write_text(summary, encoding="utf-8")
    print(summary)
    return {"median_events_per_sec": medians, "vector_map_speedup": speedup}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build/clang-release")
    parser.add_argument("--input", type=pathlib.Path, default=ROOT / "data/normalized/events.bin")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "results/stage5-preliminary")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--repeat", type=int, default=50)
    args = parser.parse_args()
    try:
        compare(args.build_dir.resolve(), args.input.resolve(), args.output.resolve(), args.runs, args.repeat)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Core comparison failed: {error}\n")
