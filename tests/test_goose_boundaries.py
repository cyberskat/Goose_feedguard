"""Additional valid int64/uint64 and empty-book boundary fixtures."""
import json
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest
from test_goose import ROOT, fix_crc


class BoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="feedguard-boundary-", dir=ROOT / "build")
        self.path = pathlib.Path(self.temp.name) / "boundary.bin"
        self.golden = bytes.fromhex((ROOT / "tests/fixtures/events-v1-golden.hex").read_text())
        self.env = os.environ.copy()
        self.env["PATH"] = "C:/msys64/clang64/bin;" + self.env["PATH"]

    def tearDown(self):
        self.temp.cleanup()

    def compare(self, data):
        self.path.write_bytes(data)
        runs = [(ROOT / "build/clang-release/book_cpp.exe", ["--engine", "map"]),
                (ROOT / "build/clang-release/book_cpp.exe", ["--engine", "vector"]),
                (ROOT / "goose/orderbook.exe", []), (ROOT / "build/goose-debug/orderbook.exe", [])]
        expected = None
        for binary, options in runs:
            result = subprocess.run([str(binary), "--input", str(self.path), *options], cwd=ROOT,
                                    env=self.env, capture_output=True, text=True, encoding="utf-8", timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            report.pop("engine")
            if expected is None:
                expected = report
            else:
                self.assertEqual(report, expected)
        return expected

    def resized(self, payload, count, markets):
        data = bytearray(self.golden[:128] + payload)
        struct.pack_into("<QQQ", data, 40, count, markets, len(payload))
        return fix_crc(data)

    def test_full_uint64_ids_and_int64_levels_remain_exact_in_json(self):
        data = bytearray(self.golden)
        struct.pack_into("<IIqq", data, 16, 18, 18, 1, 1)
        struct.pack_into("<QQQQ", data, 600+16, 2**64-1, 2**64-1, 2**64-1, 2**64-1)
        struct.pack_into("<qqqq", data, 600+72, 1, 2**63-1, 2**63-1, 1)
        report = self.compare(fix_crc(data))
        last = report["last_valid_before_reset"]
        self.assertEqual(last["u"], 2**64-1)
        self.assertEqual(last["seq"], 2**64-1)
        self.assertEqual(last["asks"], [[1,2**63-1],[2**63-1,1]])

    def test_reset_only_has_no_archived_state(self):
        report = self.compare(self.resized(self.golden[128:200], 1, 0))
        self.assertFalse(report["current"]["valid"])
        self.assertIsNone(report["last_valid_before_reset"])
        self.assertEqual(report["market_events"], 0)

    def test_empty_valid_snapshot_is_preserved_as_valid_not_null(self):
        snapshot = bytearray(self.golden[200:272])
        struct.pack_into("<I", snapshot, 0, 72)
        struct.pack_into("<II", snapshot, 64, 0, 0)
        report = self.compare(self.resized(self.golden[128:200] + snapshot + self.golden[456:528], 3, 1))
        self.assertFalse(report["current"]["valid"])
        last = report["last_valid_before_reset"]
        self.assertTrue(last["valid"])
        self.assertEqual(last["bids"], [])
        self.assertEqual(last["asks"], [])
        self.assertIsNone(last["spread"])


if __name__ == "__main__":
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(BoundaryTests))
    report = {"tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors)}
    (ROOT / "build/goose-boundaries-stage6.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    raise SystemExit(0 if result.wasSuccessful() else 1)
