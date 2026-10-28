"""Verify saved results against raw native responses and latency distributions."""
import argparse
import csv
import gzip
import hashlib
import json
import pathlib
import statistics
from statistics_tools import quantiles

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(folder, freeze=False, portable=False):
    env = json.loads((folder/"environment.json").read_text())
    rows = list(csv.DictReader((folder/"results.csv").open(newline="", encoding="utf-8")))
    raw = json.loads((folder/"raw_runs.json").read_text())
    key = lambda row: (row["dataset"], row["mode"], row["engine"], int(row["run"]))
    records = {key(row): row for row in raw}
    if len(records) != len(raw) or len(rows) != len(raw) or len(rows) != env["measured_runs"]*12:
        raise ValueError("Wrong number of measurements or duplicate run keys")
    if len({key(row) for row in rows}) != len(rows):
        raise ValueError("Duplicate CSV measurement")
    checked_samples = 0
    for row in rows:
        source = records[key(row)]
        for field in ("events", "market_events", "elapsed_ns"):
            if int(row[field]) != source[field]:
                raise ValueError(f"Wrong raw {field}")
        expected = env["verification_replays"][row["dataset"]][row["engine"]]
        if row["checksum"] != expected["checksum"] or row["last_valid_checksum"] != expected["last_valid_before_reset"]["checksum"]:
            raise ValueError("Wrong result checksum")
        if float(row["events_per_sec"]) != source["events"]*1e9/source["elapsed_ns"]:
            raise ValueError("Wrong throughput calculation")
        if float(row["peak_memory_mb"]) != source["peak_working_set_bytes"]/1e6:
            raise ValueError("Wrong memory calculation")
        if source["latencies_ns"] or source["timer_samples_ns"] or source["latency_instrumented"]:
            raise ValueError("Throughput process was instrumented")
        if row["mode"] == "core":
            path = folder/"latencies"/f"{row['dataset']}-{row['engine']}-{int(row['run']):02d}.json.gz"
            with gzip.open(path, "rt", encoding="utf-8") as input:
                samples = json.load(input)
            if samples["last_valid_checksum"] != row["last_valid_checksum"] or len(samples["latencies_ns"]) != source["events"]:
                raise ValueError("Wrong latency companion")
            q, timer = quantiles(samples["latencies_ns"]), quantiles(samples["timer_samples_ns"])
            for label in ("p50", "p99", "p999"):
                if float(row[label+"_us"]) != q[label]/1000:
                    raise ValueError("Wrong percentile calculation")
            if float(row["timer_p50_ns"]) != timer["p50"] or float(row["timer_p99_ns"]) != timer["p99"]:
                raise ValueError("Wrong timer calibration")
            checked_samples += len(samples["latencies_ns"])
        elif any(row[field] for field in ("p50_us", "p99_us", "p999_us", "timer_p50_ns", "timer_p99_ns")):
            raise ValueError("Offline replay must not claim core latency percentiles")
    snapshot = folder/"source_snapshot"
    if freeze:
        snapshot.mkdir(exist_ok=False)
        for relative, checksum in env["source_hashes"].items():
            original = ROOT/relative
            if digest(original) != checksum:
                raise ValueError("Cannot snapshot a source changed since measurement")
            target = snapshot/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(original.read_bytes())
    for relative, checksum in env["source_hashes"].items():
        path = snapshot/relative if snapshot.exists() else ROOT/relative
        if digest(path) != checksum:
            raise ValueError("Source SHA-256 mismatch")
    if portable:
        original = next(d["binary"] for d in env["datasets"] if d["name"] == "real").replace("\\", "/")
        suffix = "/data/normalized/events.bin"
        if not original.endswith(suffix):
            raise ValueError("Cannot identify the recorded project root")
        original_root = original[:-len(suffix)]
        for filename, checksum in env["input_hashes"].items():
            normalized = filename.replace("\\", "/")
            if not normalized.startswith(original_root + "/"):
                raise ValueError("Input outside the recorded project root")
            relative = pathlib.Path(normalized[len(original_root)+1:])
            if ".." in relative.parts or digest(ROOT/relative) != checksum:
                raise ValueError("Portable input SHA-256 mismatch")
        packaged_log = folder/"build_logs/stage7-goose-build.log"
        if digest(packaged_log) != env["build_log_sha256"]:
            raise ValueError("Packaged build log SHA-256 mismatch")
    else:
        for filename, checksum in {**env["binary_and_log_hashes"], **env["input_hashes"]}.items():
            if digest(pathlib.Path(filename)) != checksum:
                raise ValueError("Input/binary SHA-256 mismatch")
    groups = []
    for dataset in ("real", "synthetic"):
        for mode in ("core", "offline"):
            for engine in ("cpp_map", "cpp_vector", "goose"):
                group = [r for r in rows if (r["dataset"],r["mode"],r["engine"]) == (dataset,mode,engine)]
                if sorted(int(r["run"]) for r in group) != list(range(1, env["measured_runs"]+1)):
                    raise ValueError("Incomplete measurement group")
                def median(field):
                    values = [float(r[field]) for r in group if r[field]]
                    return statistics.median(values) if values else None
                p50 = median("p50_us")
                groups.append({"dataset": dataset, "mode": mode, "engine": engine,
                    "median_events_per_sec": median("events_per_sec"),
                    "median_p50_us": p50, "median_p99_us": median("p99_us"), "median_p999_us": median("p999_us"),
                    "max_peak_memory_mb": max(float(r["peak_memory_mb"]) for r in group),
                    "median_timer_p50_ns": median("timer_p50_ns"), "median_timer_p99_ns": median("timer_p99_ns"),
                    "clock_resolution_ns": median("clock_resolution_ns"),
                    "p50_at_or_below_clock_resolution": p50 is not None and p50*1000 <= median("clock_resolution_ns")})
    comparisons = []
    for dataset in ("real", "synthetic"):
        for mode in ("core", "offline"):
            baseline = next(g for g in groups if (g["dataset"],g["mode"],g["engine"]) == (dataset,mode,"cpp_vector"))
            goose = next(g for g in groups if (g["dataset"],g["mode"],g["engine"]) == (dataset,mode,"goose"))
            comparisons.append({"dataset":dataset,"mode":mode,
                "goose_vs_optimized_cpp_throughput":goose["median_events_per_sec"]/baseline["median_events_per_sec"],
                "p99_reduction_percent":(1-goose["median_p99_us"]/baseline["median_p99_us"])*100 if mode=="core" else None})
    return {"validated_rows":len(rows), "validated_latency_samples":checked_samples,
            "binary_verification":"recorded_hashes_only" if portable else "measured_binary_hashes_verified",
            "source_snapshot":snapshot.exists(),"groups":groups,"comparisons":comparisons,
            "latency_note":"A zero sample is below the clock quantum, not zero processing time; raw overhead is not subtracted."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results",type=pathlib.Path)
    parser.add_argument("--freeze-sources",action="store_true")
    parser.add_argument("--portable",action="store_true",help="Audit shipped evidence; original measured executable hashes remain provenance only")
    parser.add_argument("--output",type=pathlib.Path)
    args = parser.parse_args()
    report = audit(args.results.resolve(),args.freeze_sources,args.portable)
    path = args.output or args.results/"audit.json"
    if path.exists():
        raise FileExistsError("Refusing to overwrite audit.json")
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open("x",encoding="utf-8") as output:
        json.dump(report,output,indent=2)
        output.write("\n")
    print(json.dumps(report,indent=2))
