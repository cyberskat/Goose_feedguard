"""Create a labelled synthetic stress dataset independently; retain real input."""
import hashlib
import json
import pathlib
import sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_goose import create_stress


def prepare(destination):
    destination.mkdir(parents=True, exist_ok=False)
    binary, raw, metadata = create_stress(destination)
    datasets = [
        {"name": "real", "kind": "recorded_bybit", "binary": str(ROOT / "data/normalized/events.bin"),
         "capture": str(ROOT / "data/raw/btcusdt-20261027-linear-x25519.jsonl"),
         "metadata": str(ROOT / "data/metadata/btcusdt-linear-20261027.json")},
        {"name": "synthetic", "kind": "artificial_stress", "binary": str(binary),
         "capture": str(raw), "metadata": str(metadata)}]
    for dataset in datasets:
        dataset["sha256"] = hashlib.sha256(pathlib.Path(dataset["binary"]).read_bytes()).hexdigest()
        dataset["capture_sha256"] = hashlib.sha256(pathlib.Path(dataset["capture"]).read_bytes()).hexdigest()
    with (destination / "datasets.json").open("x", encoding="utf-8") as output:
        json.dump(datasets, output, indent=2)
        output.write("\n")
    return datasets


if __name__ == "__main__":
    destination = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else ROOT / "data/benchmark-stage7"
    print(json.dumps(prepare(destination), indent=2))
