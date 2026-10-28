"""Independent Python audit of the v1 binary against its source capture/metadata."""
import argparse
import decimal
import hashlib
import json
import pathlib
import struct
import zlib

HEADER = struct.Struct("<8sIIIIqqQQQII16s8s32s")
RECORD = struct.Struct("<IB3xQQQQQqqII")


def verify(binary_path, raw_path, metadata_path):
    if not __debug__:
        raise ValueError("Run this audit without Python -O; assertions must be enabled")
    if binary_path.stat().st_size > 1024**3:
        raise ValueError("binary exceeds the v1 reader's 1 GiB limit")
    data = binary_path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("truncated header")
    (magic, version, header_size, price_decimals, quantity_decimals, tick, step,
     count, market_count, payload_size, payload_crc, header_crc, symbol, category,
     metadata_sha) = HEADER.unpack_from(data)
    assert (magic, version, header_size) == (b"BFGDEVT1", 1, 128)
    assert symbol == b"BTCUSDT" + bytes(9) and category == b"linear" + bytes(2)
    header = bytearray(data[:128])
    struct.pack_into("<I", header, 68, 0)
    assert zlib.crc32(header) == header_crc
    assert payload_size == len(data) - 128
    assert zlib.crc32(data[128:]) == payload_crc
    metadata_bytes = metadata_path.read_bytes()
    assert hashlib.sha256(metadata_bytes).digest() == metadata_sha
    meta = json.loads(metadata_bytes)
    assert meta["retCode"] == 0 and meta["result"]["category"] == "linear"
    instrument, = meta["result"]["list"]
    assert instrument["symbol"] == "BTCUSDT"
    decimal.getcontext().prec = 80

    def units(value, scale):
        result = decimal.Decimal(value) * (decimal.Decimal(10) ** scale)
        assert result.is_finite() and result == result.to_integral_value()
        integer = int(result)
        assert -(2**63) <= integer < 2**63
        return integer

    for field, scale, integer in [(instrument["priceFilter"]["tickSize"], price_decimals, tick),
                                  (instrument["lotSizeFilter"]["qtyStep"], quantity_decimals, step)]:
        assert scale == max(0, -decimal.Decimal(field).normalize().as_tuple().exponent)
        assert units(field, scale) == integer
    offset, events, markets, levels = 128, 0, 0, 0
    with raw_path.open(encoding="utf-8") as raw:
        for line in raw:
            row = json.loads(line)
            kind = row["kind"]
            if kind in ("control", "collector_stop"):
                continue
            if kind not in ("session_start", "disconnect", "snapshot", "delta"):
                raise ValueError(f"invalid capture kind {kind}")
            record = RECORD.unpack_from(data, offset)
            length, event_kind, session, u, seq, ts, cts, utc, steady, nb, na = record
            assert data[offset + 5:offset + 8] == bytes(3)
            assert session == row["session"] and utc == row["recv_utc_ns"]
            expected_levels = []
            if kind in ("session_start", "disconnect"):
                assert (event_kind, u, seq, ts, cts, steady, nb, na) == (3, 0, 0, 0, 0, 0, 0, 0)
            else:
                source = row["raw"]
                book = source["data"]
                assert row["accepted"] is True
                assert source["type"] == kind and source["topic"] == "orderbook.50.BTCUSDT"
                assert (event_kind, u, seq, ts, cts, steady, nb, na) == (
                    1 if kind == "snapshot" else 2, book["u"], book["seq"], source["ts"],
                    source.get("cts", 0), row["recv_steady_ns"], len(book["b"]), len(book["a"]))
                for side in ("b", "a"):
                    expected_levels.extend((units(p, price_decimals), units(q, quantity_decimals)) for p, q in book[side])
                markets += 1
            assert length == 72 + 16 * len(expected_levels)
            actual = [struct.unpack_from("<qq", data, offset + 72 + i * 16) for i in range(nb + na)]
            assert actual == expected_levels
            levels += len(actual)
            offset += length
            events += 1
    assert offset == len(data) and events == count and markets == market_count
    return {"events": events, "market_events": markets, "resets": events - markets,
            "levels": levels, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "metadata_sha256": metadata_sha.hex(), "payload_crc32": f"{payload_crc:08x}"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("metadata", type=pathlib.Path)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.binary, args.capture, args.metadata), indent=2))
    except (AssertionError, ValueError, KeyError, TypeError, OSError, struct.error) as error:
        parser.exit(1, f"Independent audit failed: {type(error).__name__}: {error}\n")
