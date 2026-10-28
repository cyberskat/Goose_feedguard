"""Compiled Goose correctness suite. Requires the recorded real capture locally."""
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from verify_book import reference

HEADER = struct.Struct("<8sIIIIqqQQQII16s8s32s")
RECORD = struct.Struct("<IB3xQQQQQqqII")
STATS = {"reader_rejections": 0, "trace_states_checked": 0, "self_test_assertions": {}}


def fix_crc(data):
    struct.pack_into("<I", data, 64, zlib.crc32(data[128:]))
    struct.pack_into("<I", data, 68, 0)
    struct.pack_into("<I", data, 68, zlib.crc32(data[:128]))
    return data


def create_stress(folder):
    metadata = ROOT / "tests/fixtures/instrument-synthetic.json"
    raw_path, binary_path = folder / "stress.jsonl", folder / "stress.bin"
    body = bytearray()
    events = markets = session = 0

    def decimal_text(value, scale):
        factor = 10**scale
        return f"{value // factor}.{value % factor:0{scale}d}"

    with raw_path.open("w", encoding="utf-8", newline="\n") as raw:
        def emit(kind, index, bids=(), asks=(), u=0):
            nonlocal events, markets
            utc = 1000 + events
            row = {"schema": 1, "session": session, "kind": kind, "recv_utc_ns": utc}
            market = kind in ("snapshot", "delta")
            seq, ts, steady = (index * 17, index + 1, utc) if market else (0, 0, 0)
            if market:
                row.update({"accepted": True, "recv_steady_ns": steady,
                            "raw": {"topic": "orderbook.50.BTCUSDT", "type": kind, "ts": ts,
                                    "data": {"s": "BTCUSDT", "u": u, "seq": seq,
                                             "b": [[decimal_text(p, 1), decimal_text(q, 3)] for p, q in bids],
                                             "a": [[decimal_text(p, 1), decimal_text(q, 3)] for p, q in asks]}}})
                markets += 1
            raw.write(json.dumps(row, separators=(",", ":")) + "\n")
            event_kind = 1 if kind == "snapshot" else 2 if kind == "delta" else 3
            body.extend(RECORD.pack(72 + (len(bids) + len(asks))*16, event_kind, session, u, seq, ts, 0,
                                    utc, steady, len(bids), len(asks)))
            for p, q in list(bids) + list(asks):
                body.extend(struct.pack("<qq", p, q))
            events += 1
        for i in range(100000):
            if i % 1000 == 0:
                if session:
                    emit("disconnect", i)
                session += 1
                emit("session_start", i)
            elif i % 1000 == 1 or i % 500 == 0:
                emit("snapshot", i, [(10000, 2), (9995, 4)], [(11000, 6), (11005, 8)], 1)
            else:
                emit("delta", i, [(10000 - 5*(i % 8), 2*(i % 13))],
                     [(11000 + 5*((i * 7) % 8), 2*((i + 3) % 11))], i + 2)
        emit("disconnect", 100000)
        raw.write(json.dumps({"schema": 1, "session": session, "kind": "collector_stop", "recv_utc_ns": 1000+events})+"\n")
    header = bytearray(HEADER.pack(b"BFGDEVT1", 1, 128, 1, 3, 5, 2, events, markets, len(body),
                                  zlib.crc32(body), 0, b"BTCUSDT", b"linear", hashlib.sha256(metadata.read_bytes()).digest()))
    struct.pack_into("<I", header, 68, zlib.crc32(header))
    binary_path.write_bytes(header + body)
    STATS["synthetic_events"] = events
    STATS["synthetic_market_events"] = markets
    return binary_path, raw_path, metadata


class GooseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="feedguard-goose-", dir=ROOT / "build")
        cls.folder = pathlib.Path(cls.temporary.name)
        cls.env = os.environ.copy()
        cls.env["PATH"] = "C:/msys64/clang64/bin;" + cls.env["PATH"]
        cls.binaries = {"release": ROOT / "goose/orderbook.exe", "debug": ROOT / "build/goose-debug/orderbook.exe"}
        cls.cpp = ROOT / "build/clang-release/book_cpp.exe"
        for binary in [*cls.binaries.values(), cls.cpp]:
            if not binary.is_file():
                raise FileNotFoundError(f"Build required: {binary}")
        cls.golden = bytes.fromhex((ROOT / "tests/fixtures/events-v1-golden.hex").read_text())
        cls.golden_path = cls.folder / "golden.bin"
        cls.golden_path.write_bytes(cls.golden)
        cls.stress = create_stress(cls.folder)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, binary, args, timeout=90):
        return subprocess.run([str(binary), *map(str, args)], cwd=ROOT, env=self.env,
                              capture_output=True, text=True, encoding="utf-8", timeout=timeout)

    def assert_rejected(self, binary, data):
        path = self.folder / "invalid.bin"
        path.write_bytes(data)
        result = self.invoke(binary, ["--input", path])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertTrue(result.stderr)
        STATS["reader_rejections"] += 1

    def audit_trace(self, name, binary, source, raw, metadata):
        trace = self.folder / (name + ".trace.jsonl")
        with trace.open("w", encoding="utf-8", newline="\n") as output:
            run = subprocess.run([str(binary), "--input", str(source), "--trace"], cwd=ROOT,
                                 env=self.env, stdout=output, stderr=subprocess.PIPE,
                                 text=True, encoding="utf-8", timeout=90)
        self.assertEqual(run.returncode, 0, run.stderr)
        with trace.open(encoding="utf-8") as lines:
            def compare(expected):
                line = next(lines, None)
                self.assertIsNotNone(line, f"missing trace event {expected['events']}")
                actual = json.loads(line)
                self.assertEqual(actual.pop("engine"), "goose")
                self.assertEqual(actual, expected, f"event {expected['events']}")
                STATS["trace_states_checked"] += 1
            expected = reference(raw, metadata, on_state=compare)
            self.assertEqual(lines.read(), "")
        for engine in ("map", "vector"):
            cpp = self.invoke(self.cpp, ["--input", source, "--engine", engine])
            self.assertEqual(cpp.returncode, 0, cpp.stderr)
            actual = json.loads(cpp.stdout)
            actual.pop("engine")
            self.assertEqual(actual, expected)
        final = self.invoke(binary, ["--input", source])
        self.assertEqual(final.returncode, 0, final.stderr)
        actual = json.loads(final.stdout)
        actual.pop("engine")
        self.assertEqual(actual, expected)
        return expected

    def test_01_native_kernel_self_tests(self):
        for name, binary in self.binaries.items():
            result = self.invoke(binary, ["--self-test"])
            self.assertEqual(result.returncode, 0, result.stderr)
            checks = json.loads(result.stdout)["self_tests_passed"]
            self.assertEqual(checks, 46)
            STATS["self_test_assertions"][name] = checks

    def test_02_golden_every_state(self):
        for name, binary in self.binaries.items():
            expected = self.audit_trace(name + "-golden", binary, self.golden_path,
                                        ROOT / "tests/fixtures/capture-golden.jsonl",
                                        ROOT / "tests/fixtures/instrument-synthetic.json")
            self.assertEqual(expected["history_checksum"], "6abb994ed1892e86")

    def test_03_all_truncations_and_single_byte_corruptions(self):
        for name, binary in self.binaries.items():
            for length in range(len(self.golden)):
                with self.subTest(build=name, truncation=length):
                    self.assert_rejected(binary, self.golden[:length])
            for at in range(len(self.golden)):
                corrupted = bytearray(self.golden)
                corrupted[at] ^= 1
                with self.subTest(build=name, flipped_byte=at):
                    self.assert_rejected(binary, corrupted)
            self.assert_rejected(binary, self.golden + b"\0")
            print(f"{name}: checked every golden truncation and byte position", flush=True)

    def test_04_forged_checksums_reach_semantic_guards(self):
        cases = [(8,"I",2),(16,"I",19),(24,"q",0),(40,"Q",2**64-1),(48,"Q",99),
                 (56,"Q",1),(128,"I",2**32-1),(132,"B",255),(133,"B",1),
                 (200+8,"Q",0),(200+64,"I",4097),(200+16,"Q",0),(200+4,"B",2),
                 (200+72,"q",1006),(200+72+8,"q",-1),(200+72+8,"q",0),
                 (200+72+16,"q",1005),(200+48,"q",-1),(336+16,"Q",1)]
        for at, fmt, value in cases:
            data = bytearray(self.golden)
            struct.pack_into("<" + fmt, data, at, value)
            fix_crc(data)
            for binary in [*self.binaries.values(), self.cpp]:
                with self.subTest(binary=binary.name, offset=at):
                    self.assert_rejected(binary, data)

    def test_05_stress_every_state(self):
        for name, binary in self.binaries.items():
            result = self.audit_trace(name + "-stress", binary, *self.stress)
            STATS["stress_history_checksum"] = result["history_checksum"]

    def test_06_real_every_state(self):
        source = ROOT / "data/normalized/events.bin"
        raw = ROOT / "data/raw/btcusdt-20261027-linear-x25519.jsonl"
        metadata = ROOT / "data/metadata/btcusdt-linear-20261027.json"
        for name, binary in self.binaries.items():
            result = self.audit_trace(name + "-real", binary, source, raw, metadata)
            self.assertEqual(result["market_events"], 10000)
            self.assertEqual(result["history_checksum"], "133ad3f85b1d7b87")
            STATS["real_history_checksum"] = result["history_checksum"]

    def test_07_cli_rejects_invalid_arguments_and_missing_files(self):
        for binary in self.binaries.values():
            for args in (["--bad"], ["--input"], ["--input", self.golden_path, "--bad"],
                         ["--input", self.folder / "missing.bin"]):
                result = self.invoke(binary, args)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
            self.assertEqual(self.invoke(binary, ["--help"]).returncode, 0)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(GooseTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {"tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors), **STATS}
    path = ROOT / "build/goose-correctness-stage6.json"
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if result.wasSuccessful() else 1)
