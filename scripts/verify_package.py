"""Verify the per-file hashes in a prepared review snapshot."""
import argparse
import hashlib
import json
import pathlib


def verify(root):
    root = root.resolve()
    manifest = json.loads((root/"RELEASE_MANIFEST.json").read_text(encoding="utf-8"))
    size = 0
    for relative, record in manifest["files"].items():
        path = (root/relative).resolve()
        if not path.is_relative_to(root) or path.is_symlink() or not path.is_file():
            raise ValueError(f"Invalid package path: {relative}")
        body = path.read_bytes()
        if len(body) != record["bytes"] or hashlib.sha256(body).hexdigest() != record["sha256"]:
            raise ValueError(f"Package hash mismatch: {relative}")
        size += len(body)
    return {"verified_files":len(manifest["files"]),"verified_bytes":size,
            "language":manifest["language"],"publication_status":manifest["publication_status"]}


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root",type=pathlib.Path,nargs="?",default=pathlib.Path(__file__).resolve().parents[1])
    args=parser.parse_args()
    print(json.dumps(verify(args.root),indent=2))
