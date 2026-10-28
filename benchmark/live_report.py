"""Live connection stability only; does not compare language performance."""
import argparse
import json
import pathlib
import sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from validate_capture import validate


def report(path, expected):
    validated = validate(path, expected)
    records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    market = [r for r in records if r["kind"] in ("snapshot", "delta")]
    disconnects = [r["detail"] for r in records if r["kind"] == "disconnect"]
    return {"mode": "live_stability_only", **validated,
            "reconnections": max(0, validated["records"]["session_start"] - 1),
            "disconnect_reasons": disconnects,
            "pong_responses": sum(r["kind"] == "control" and r["raw"].get("ret_msg") == "pong" for r in records),
            "observed_market_span_seconds": (market[-1]["recv_steady_ns"] - market[0]["recv_steady_ns"]) / 1e9,
            "collector_stop": records[-1]["detail"],
            "exchange_to_local_latency_measured": False,
            "language_speed_comparison": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--expected", type=int, default=2000)
    args = parser.parse_args()
    print(json.dumps(report(args.capture, args.expected), indent=2))
