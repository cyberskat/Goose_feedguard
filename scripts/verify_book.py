"""Audit C++ replay JSON against an independent replay of the original JSONL."""
import argparse
import decimal
import hashlib
import json
import pathlib
import struct

OFFSET = 14695981039346656037
PRIME = 1099511628211
MASK = 2**64 - 1


def fnv(data, value=OFFSET):
    for byte in data:
        value = ((value ^ byte) * PRIME) & MASK
    return value


def reference(capture, metadata, on_state=None):
    decimal.getcontext().prec = 80
    document = json.loads(metadata.read_bytes())
    if document["retCode"] != 0 or document["result"]["category"] != "linear":
        raise ValueError("Unexpected instrument metadata")
    instrument, = document["result"]["list"]
    if instrument["symbol"] != "BTCUSDT":
        raise ValueError("Unexpected symbol")
    tick = decimal.Decimal(instrument["priceFilter"]["tickSize"])
    step = decimal.Decimal(instrument["lotSizeFilter"]["qtyStep"])
    pd = max(0, -tick.normalize().as_tuple().exponent)
    qd = max(0, -step.normalize().as_tuple().exponent)
    price_factor, quantity_factor = decimal.Decimal(10)**pd, decimal.Decimal(10)**qd
    tick_units, step_units = int(tick * price_factor), int(step * quantity_factor)
    prefix = b"BFGBOOK1" + struct.pack("<IIqq", pd, qd, tick_units, step_units)
    bids, asks = {}, {}
    valid, session, u, seq = False, 0, 0, 0

    def export():
        bid_levels = [[p, bids[p]] for p in sorted(bids, reverse=True)]
        ask_levels = [[p, asks[p]] for p in sorted(asks)]
        wire = prefix + struct.pack("<BQQQQQ", valid, session, u, seq, len(bids), len(asks))
        wire += b"".join(struct.pack("<qq", p, q) for p, q in bid_levels + ask_levels)
        best_bid = bid_levels[0] if valid and bid_levels else None
        best_ask = ask_levels[0] if valid and ask_levels else None
        return {"valid": valid, "session": session, "u": u, "seq": seq,
                "checksum": f"{fnv(wire):016x}", "best_bid": best_bid, "best_ask": best_ask,
                "spread": best_ask[0] - best_bid[0] if best_bid and best_ask else None,
                "bids": bid_levels, "asks": ask_levels}

    history, last = fnv(b"BFGTRACE1"), None
    events, markets, resets = 0, 0, 0
    with capture.open(encoding="utf-8") as source:
        for line in source:
            row = json.loads(line)
            kind = row["kind"]
            if kind in ("control", "collector_stop"):
                continue
            if kind in ("session_start", "disconnect"):
                if valid:
                    last = export()
                bids, asks = {}, {}
                valid, session, u, seq = False, row["session"], 0, 0
                event_kind = 3
                resets += 1
            elif kind in ("snapshot", "delta"):
                raw = row["raw"]
                data = raw["data"]
                if not row["accepted"] or raw["topic"] != "orderbook.50.BTCUSDT" or data["s"] != "BTCUSDT":
                    raise ValueError("Unexpected market event")
                if row["session"] != session or (kind == "delta" and not valid):
                    raise ValueError("Market data outside initialized session")
                if kind == "snapshot":
                    bids, asks = {}, {}
                for side, key in ((bids, "b"), (asks, "a")):
                    for p, q in data[key]:
                        price, quantity = decimal.Decimal(p)*price_factor, decimal.Decimal(q)*quantity_factor
                        if price != price.to_integral_value() or quantity != quantity.to_integral_value():
                            raise ValueError("Lost precision")
                        price, quantity = int(price), int(quantity)
                        if price <= 0 or quantity < 0 or price % tick_units or quantity % step_units:
                            raise ValueError("Invalid level")
                        if quantity:
                            side[price] = quantity
                        else:
                            side.pop(price, None)
                valid, u, seq = True, data["u"], data["seq"]
                event_kind = 1 if kind == "snapshot" else 2
                markets += 1
            else:
                raise ValueError(f"Unexpected journal record {kind}")
            state = export()
            history = fnv(struct.pack("<BQQQQ", event_kind, session, u, seq, int(state["checksum"], 16)), history)
            events += 1
            if on_state is not None:
                on_state({"schema": 1, "mode": "verify_replay", "events": events, "market_events": markets,
                          "resets": resets, "price_decimals": pd, "quantity_decimals": qd,
                          "checksum": state["checksum"], "history_checksum": f"{history:016x}",
                          "current": state, "last_valid_before_reset": last})
    state = export()
    return {"schema": 1, "mode": "verify_replay", "events": events, "market_events": markets,
            "resets": resets, "price_decimals": pd, "quantity_decimals": qd,
            "checksum": state["checksum"], "history_checksum": f"{history:016x}",
            "current": state, "last_valid_before_reset": last}


def verify(report, capture, metadata):
    expected = reference(capture, metadata)
    actual = json.loads(report.read_bytes())
    for key, value in expected.items():
        if key not in actual or actual[key] != value:
            raise ValueError(f"Mismatch in replay field {key}")
    return {"verified": True, "events": expected["events"], "market_events": expected["market_events"],
            "checksum": expected["checksum"], "history_checksum": expected["history_checksum"],
            "last_valid_checksum": expected["last_valid_before_reset"]["checksum"] if expected["last_valid_before_reset"] else None,
            "capture_sha256": hashlib.sha256(capture.read_bytes()).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("metadata", type=pathlib.Path)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.report, args.capture, args.metadata), indent=2))
    except (ValueError, KeyError, TypeError, OSError, decimal.DecimalException, struct.error) as error:
        parser.exit(1, f"Book audit failed: {error}\n")
