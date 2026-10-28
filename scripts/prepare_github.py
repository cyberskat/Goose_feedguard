"""Export an English review snapshot. Creates no repository, commit, or remote."""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import struct
import zipfile
from verify_package import verify

ROOT=pathlib.Path(__file__).resolve().parents[1]
PUBLIC_DOCS=("technical_report.md","reproduction.md","binary_format.md","pilot_proposal.md","publication.md",
             "live-stage7.json","benchmark-stage7-audit.json")
DATA_FILES=("data/raw/btcusdt-20261027-linear-x25519.jsonl",
    "data/raw/live-stage7-20261028.jsonl","data/normalized/events.bin","data/metadata/btcusdt-linear-20261027.json")
PUBLIC_IGNORE="""/build/
/.venv/
/.tools/
/.idea/
__pycache__/
*.exe
*.dll
CMakeUserPresets.json
/data/benchmark-*/
/data/raw/*
!/data/raw/btcusdt-20261027-linear-x25519.jsonl
!/data/raw/live-stage7-20261028.jsonl
/data/normalized/*
!/data/normalized/events.bin
/results/*
!/results/stage7-20261028/
"""


def check_links(root):
    for path in root.rglob("*.md"):
        for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)",path.read_text(encoding="utf-8-sig")):
            if target.startswith(("https://","http://","mailto:","#")):
                continue
            target=target.split("#",1)[0].strip("<>")
            if not target:
                continue
            resolved=(path.parent/target).resolve()
            if not resolved.is_relative_to(root.resolve()) or not resolved.exists():
                raise ValueError(f"Broken package link in {path.relative_to(root)}: {target}")


def prepare(destination):
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite {destination}")
    selected=set()
    for name in ("README.md","CMakeLists.txt","CMakePresets.json",".gitattributes"):
        selected.add(ROOT/name)
    for directory,extensions in (("include",{'.hpp','.h'}),("src",{'.cpp','.c'}),
        ("goose",{'.goose','.build'}),("benchmark",{'.py','.txt'}),("scripts",{'.py','.ps1'}),
        ("tests",{'.cpp','.hpp','.py','.json','.jsonl','.hex','.md'})):
        selected.update(p for p in (ROOT/directory).rglob('*') if p.is_file() and p.suffix in extensions and '__pycache__' not in p.parts)
    selected.update(ROOT/"docs"/name for name in PUBLIC_DOCS)
    selected.update((ROOT/"results/stage7-20261028").rglob('*'))
    selected.update(ROOT/name for name in DATA_FILES)
    selected={p for p in selected if p.is_file()}
    required=[ROOT/"docs"/name for name in PUBLIC_DOCS]
    required += [ROOT/"results/stage7-20261028/figures"/name for name in ("throughput.png","latency_p99.png","figure_manifest.json")]
    figures=required[-3:-1]
    required += [ROOT/name for name in DATA_FILES]
    required += [ROOT/path for path in re.findall(r'\b(?:src|tests)/[A-Za-z0-9_./-]+\.(?:cpp|c)\b',(ROOT/"CMakeLists.txt").read_text())]
    if any(not path.is_file() for path in required):
        raise ValueError("Required English documents or figures are missing")
    if not set(required).issubset(selected):
        raise ValueError("A required build/data input is outside the export selection")
    for image in figures:
        body=image.read_bytes()
        if body[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack('>II',body[16:24]) != (1600,900):
            raise ValueError("Publication figure must be a 1600x900 PNG")
    # The selection is explicit; symlinks or non-English-facing documentation are not silently copied.
    for path in selected:
        if path.is_symlink() or not path.resolve().is_relative_to(ROOT):
            raise ValueError("Invalid export source")
        if path.suffix=='.md' and re.search('[А-Яа-яЁё]',path.read_text(encoding='utf-8-sig')):
            raise ValueError(f"Non-English document in publication scope: {path}")
    root=destination/"bybit-feedguard"
    root.mkdir(parents=True,exist_ok=False)
    for source in sorted(selected):
        target=root/source.relative_to(ROOT)
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(source.read_bytes())
    (root/".gitignore").write_text(PUBLIC_IGNORE,encoding="utf-8")
    check_links(root)
    files={str(p.relative_to(root)).replace('\\','/'):{"bytes":p.stat().st_size,
           "sha256":hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(root.rglob('*')) if p.is_file()}
    manifest={"project":"Bybit FeedGuard","snapshot":"pre-publication review",
              "created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),"language":"en",
              "publication_status":"awaiting_owner_repository_visibility_license_approval",
              "manifest_self_excluded":True,"files":files}
    (root/"RELEASE_MANIFEST.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    verification=verify(root)
    archive=destination/"bybit-feedguard-review.zip"
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=9) as output:
        for path in sorted(root.rglob('*')):
            if path.is_file():
                output.write(path,path.relative_to(destination))
    checksum=hashlib.sha256(archive.read_bytes()).hexdigest()
    (destination/"RELEASE_SHA256.txt").write_text(checksum+"  "+archive.name+"\n",encoding="utf-8")
    result={"directory":str(root),"archive":str(archive),"archive_sha256":checksum,
            "archive_bytes":archive.stat().st_size,**verification}
    (destination/"preparation_report.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    return result


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=pathlib.Path,default=ROOT/"build/github-review")
    args=parser.parse_args()
    print(json.dumps(prepare(args.output.resolve()),indent=2))
