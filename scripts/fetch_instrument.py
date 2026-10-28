"""Fetch public Bybit instrument metadata over verified HTTPS; never overwrite."""
import argparse
import datetime
import hashlib
import json
import pathlib
import urllib.request

URL = "https://api.bybit.com/v5/market/instruments-info?category=linear&symbol=BTCUSDT"


def fetch(destination):
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite {destination}")
    request = urllib.request.Request(URL, headers={"User-Agent": "Bybit-FeedGuard/0.1"})
    with urllib.request.urlopen(request, timeout=30) as response:
        body = response.read(1024 * 1024 + 1)
    if len(body) > 1024 * 1024:
        raise ValueError("Metadata exceeds 1 MiB")
    doc = json.loads(body)
    if doc["retCode"] != 0 or doc["result"]["category"] != "linear":
        raise ValueError("Unexpected Bybit result")
    instrument, = doc["result"]["list"]
    if instrument["symbol"] != "BTCUSDT":
        raise ValueError("Unexpected instrument")
    if not isinstance(instrument["priceFilter"]["tickSize"], str) or not isinstance(instrument["lotSizeFilter"]["qtyStep"], str):
        raise ValueError("Instrument steps must be strings")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as output:
        output.write(body)
    return {"url": URL, "received_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "path": str(destination), "sha256": hashlib.sha256(body).hexdigest(), "bytes": len(body)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        print(json.dumps(fetch(args.output), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"Metadata fetch failed: {error}\n")
