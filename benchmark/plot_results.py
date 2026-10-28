"""Render audited core metrics from existing CSV; never synthesize measurements."""
import argparse
import datetime
import hashlib
import json
import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINES = ("cpp_map", "cpp_vector", "goose")
LABELS = ("C++ map\nBaseline", "C++ vector\nOptimized", "Goose\nNative")
COLORS = ("#94A3B8", "#2859D6", "#128472")
INK, MUTED, GRID = "#14243A", "#53657B", "#E1E7EF"


def load_metrics(folder, dataset="real"):
    frame = pd.read_csv(folder / "results.csv")
    environment = json.loads((folder / "environment.json").read_text(encoding="utf-8"))
    required = {"dataset", "mode", "engine", "run", "events_per_sec", "p99_us", "clock_resolution_ns"}
    if not required.issubset(frame.columns):
        raise ValueError("Missing measurement columns")
    selected = frame[(frame["dataset"] == dataset) & (frame["mode"] == "core")]
    metrics = {}
    for engine in ENGINES:
        group = selected[selected["engine"] == engine]
        if len(group) < 10 or group["run"].duplicated().any():
            raise ValueError(f"Need at least 10 distinct runs for {engine}")
        if group[["events_per_sec", "p99_us"]].isna().any().any() or (group["events_per_sec"] <= 0).any() or (group["p99_us"] <= 0).any():
            raise ValueError("Invalid or unavailable metrics; no placeholder bars will be drawn")
        metrics[engine] = {key: {"median": float(group[key].median()), "min": float(group[key].min()), "max": float(group[key].max())}
                           for key in ("events_per_sec", "p99_us")}
    if len({int(selected[selected["engine"] == engine]["run"].count()) for engine in ENGINES}) != 1:
        raise ValueError("Different run counts across engines")
    return selected, environment, metrics


def render(folder, destination, dataset="real"):
    frame, env, metrics = load_metrics(folder, dataset)
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite {destination}")
    destination.mkdir(parents=True, exist_ok=False)
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 14, "axes.unicode_minus": False})
    measured = datetime.datetime.fromisoformat(env["created_utc"]).astimezone(datetime.timezone(datetime.timedelta(hours=3)))
    date = measured.strftime("%d %b %Y")
    cpu = env["cpu"].replace("(R)", "").replace("(TM)", "")
    throughput_ratio = metrics["goose"]["events_per_sec"]["median"] / metrics["cpp_vector"]["events_per_sec"]["median"]
    p99_change = (1 - metrics["goose"]["p99_us"]["median"] / metrics["cpp_vector"]["p99_us"]["median"]) * 100
    resolution = float(frame["clock_resolution_ns"].max()) / 1000
    source = env["verification_replays"][dataset]["cpp_vector"]
    for key, filename, title, direction, ylabel, scale in (
        ("events_per_sec", "throughput.png", "Core throughput", "Higher is better", "Million events / second", 1e6),
        ("p99_us", "latency_p99.png", "p99 apply latency", "Lower is better", "Microseconds", 1.0),
    ):
        fig = plt.figure(figsize=(16, 9), dpi=100, facecolor="white")
        fig.text(.07, .93, "BYBIT FEEDGUARD", fontsize=14, fontweight="bold", color=MUTED)
        fig.text(.07, .865, title, fontsize=32, fontweight="bold", color=INK)
        fig.text(.07, .82, f"BTCUSDT linear · Depth 50 · {direction}", fontsize=16, color=MUTED)
        if key == "events_per_sec":
            callout = f"{throughput_ratio:.2f}× throughput\nvs optimized C++"
        else:
            callout = f"{abs(p99_change):.1f}% {'lower' if p99_change >= 0 else 'higher'} p99\nvs optimized C++"
        fig.text(.94, .867, callout, ha="right", va="center", fontsize=19, fontweight="bold", color=COLORS[2],
                 bbox={"boxstyle": "round,pad=0.7", "facecolor": "#ECF7F3", "edgecolor": "none"})
        ax = fig.add_axes([.115, .285, .81, .455])
        values = [metrics[e][key]["median"] / scale for e in ENGINES]
        low = [metrics[e][key]["min"] / scale for e in ENGINES]
        high = [metrics[e][key]["max"] / scale for e in ENGINES]
        maximum = max(high)
        ax.set_ylim(0, maximum * 1.35)
        ax.set_xlim(-.65, 2.65)
        ax.set_axisbelow(True)
        ax.yaxis.grid(True, color=GRID, linewidth=1)
        bars = ax.bar(range(3), values, width=.58, color=COLORS, zorder=3)
        ax.errorbar(range(3), values, yerr=[[v-l for v,l in zip(values,low)], [h-v for h,v in zip(high,values)]],
                    fmt="none", ecolor=INK, elinewidth=1.4, capsize=6, capthick=1.4, zorder=4)
        for bar, value, peak in zip(bars, values, high):
            label = f"{value:.2f}M" if key == "events_per_sec" else f"{value:.2f} µs"
            ax.text(bar.get_x()+bar.get_width()/2, peak+maximum*.07, label, ha="center", va="bottom",
                    fontsize=22, fontweight="bold", color=INK)
        ax.set_xticks(range(3), LABELS, fontsize=17, color=INK)
        ax.tick_params(axis="x", length=0, pad=15)
        ax.tick_params(axis="y", length=0, labelcolor=MUTED, pad=8)
        ax.set_ylabel(ylabel, fontsize=14, color=MUTED, labelpad=14)
        for spine in ax.spines.values():
            spine.set_visible(False)
        fig.text(.07,.16,f"{len(frame)//3} measured runs / engine · Medians of per-run metrics · Whiskers: observed min–max",fontsize=13,color=MUTED)
        note = "Batch timing; validation and reset archival included; history checks excluded."
        if key == "p99_us":
            note = f"Separate instrumented passes · QPC quantum {resolution:g} µs · Timer overhead not subtracted."
        fig.text(.07,.12,note,fontsize=13,color=MUTED)
        fig.text(.07,.08,f"{cpu} · Clang 23.1.4 · Native -O2 · {date}",fontsize=12,color=MUTED)
        fig.text(.07,.045,f"Input: {source['market_events']:,} {'recorded market' if dataset=='real' else 'synthetic'} events + {source['resets']} resets · Full evidence: results.csv",fontsize=11,color=MUTED)
        if key == "p99_us" and min(values) <= resolution*2:
            fig.text(.94,.045,"Near timer resolution: interpret cautiously",fontsize=10,color="#9C4B16",ha="right")
        fig.savefig(destination/filename,dpi=100,facecolor="white")
        plt.close(fig)
    manifest = {"dataset":dataset,"mode":"core","csv_sha256":hashlib.sha256((folder/"results.csv").read_bytes()).hexdigest(),
                "environment_sha256":hashlib.sha256((folder/"environment.json").read_bytes()).hexdigest(),
                "width":1600,"height":900,"metrics":metrics,"throughput_vs_optimized_cpp":throughput_ratio,
                "p99_reduction_percent":p99_change,"measurement_date":measured.date().isoformat(),
                "matplotlib":matplotlib.__version__,"pandas":pd.__version__}
    (destination/"figure_manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    return manifest


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results",type=pathlib.Path,default=ROOT/"results/stage7-20261028")
    parser.add_argument("--output",type=pathlib.Path)
    parser.add_argument("--dataset",choices=("real","synthetic"),default="real")
    args=parser.parse_args()
    output=args.output or args.results/"figures"
    render(args.results.resolve(),output.resolve(),args.dataset)
