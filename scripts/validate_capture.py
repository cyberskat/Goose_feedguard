"""Validate a real collector journal. This does not benchmark an order book."""
import argparse
import collections
import hashlib
import json
import pathlib


def validate(path, expected):
    counts = collections.Counter()
    ready = False
    active_session = None
    last_session = 0
    last_steady = -1
    accepted = 0
    stopped = False
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for number, line in enumerate(source, 1):
            sha.update(line)
            record = json.loads(line)
            if stopped:
                raise ValueError(f"line {number}: data after collector_stop")
            if record["schema"] != 1 or record["recv_utc_ns"] <= 0:
                raise ValueError(f"line {number}: invalid schema or timestamp")
            kind, session = record["kind"], record["session"]
            counts[kind] += 1
            if kind == "session_start":
                if active_session is not None or session <= last_session:
                    raise ValueError(f"line {number}: invalid session start")
                active_session, last_session, ready = session, session, False
            elif kind == "disconnect":
                if session != active_session:
                    raise ValueError(f"line {number}: invalid disconnect")
                active_session, ready = None, False
            elif kind in ("snapshot", "delta", "control"):
                if session != active_session:
                    raise ValueError(f"line {number}: payload outside session")
                steady = record["recv_steady_ns"]
                if steady < last_steady:
                    raise ValueError(f"line {number}: non-monotonic receive clock")
                last_steady = steady
                raw = record["raw"]
                if not isinstance(raw, dict):
                    raise ValueError(f"line {number}: raw must be an object")
                if kind == "snapshot":
                    ready = True
                if kind in ("snapshot", "delta"):
                    if raw["topic"] != "orderbook.50.BTCUSDT" or raw["data"]["s"] != "BTCUSDT":
                        raise ValueError(f"line {number}: unexpected market")
                    if raw["type"] != kind or not ready or record["accepted"] is not True:
                        raise ValueError(f"line {number}: market event without snapshot")
                    accepted += 1
                elif record["accepted"] is not False:
                    raise ValueError(f"line {number}: control counted as market data")
            elif kind == "collector_stop":
                if active_session is not None:
                    raise ValueError(f"line {number}: unclosed session")
                stopped = True
            elif kind == "invalid":
                raise ValueError(f"line {number}: invalid message in capture")
            else:
                raise ValueError(f"line {number}: unknown record kind {kind}")
    if accepted != expected:
        raise ValueError(f"expected {expected} accepted market messages, got {accepted}")
    if not stopped or not counts["snapshot"]:
        raise ValueError("capture missing final stop or snapshot")
    return {"path": str(path), "accepted_market_messages": accepted,
            "records": dict(counts), "sha256": sha.hexdigest(), "bytes": path.stat().st_size}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--expected", type=int, default=10000)
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.capture, args.expected), indent=2))
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, f"Capture validation failed: {error}\n")
