import json
import os
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmark"))
from statistics_tools import quantiles


class StatisticsTests(unittest.TestCase):
    def test_nearest_rank_small_known_distribution(self):
        self.assertEqual(quantiles([9, 1, 3, 7, 5]), {"p50": 5, "p99": 9, "p999": 9})
        self.assertEqual(quantiles([0]), {"p50": 0, "p99": 0, "p999": 0})
        self.assertEqual(quantiles(list(range(1, 1001))), {"p50": 500, "p99": 990, "p999": 999})

    def test_invalid_samples_never_generate_statistics(self):
        for values in ([], [-1], [1.5], [True]):
            with self.assertRaises(ValueError):
                quantiles(values)


class NativeMeasurements(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.env = os.environ.copy()
        cls.env["PATH"] = "C:/msys64/clang64/bin;" + cls.env["PATH"]
        cls.input = ROOT / "data/normalized/events.bin"
        cls.programs = [([str(ROOT / "build/clang-benchmark/book_cpp.exe"), "--engine", "map"], "cpp_map"),
                        ([str(ROOT / "build/clang-benchmark/book_cpp.exe"), "--engine", "vector"], "cpp_vector"),
                        ([str(ROOT / "goose/orderbook.exe")], "goose")]

    def invoke(self, program, args):
        return subprocess.run(program + ["--input", str(self.input)] + args, cwd=ROOT, env=self.env,
                              capture_output=True, text=True, encoding="utf-8", timeout=30)

    def test_throughput_and_latency_passes_have_distinct_samples(self):
        for program, engine in self.programs:
            for mode, sampled in (("core", False), ("offline", False), ("core", True)):
                with self.subTest(engine=engine, mode=mode, sampled=sampled):
                    result = self.invoke(program, ["--measure", mode] + (["--latencies"] if sampled else []))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    row = json.loads(result.stdout)
                    self.assertEqual(row["engine"], engine)
                    self.assertEqual(row["events"], 10002)
                    self.assertEqual(row["market_events"], 10000)
                    self.assertEqual(row["checksum"], "e898d48c8118d4d9")
                    self.assertEqual(row["last_valid_checksum"], "a8bea05ec3dc51c7")
                    self.assertGreater(row["elapsed_ns"], 0)
                    self.assertGreater(row["peak_working_set_bytes"], 0)
                    self.assertGreater(row["clock_frequency_hz"], 0)
                    self.assertEqual(row["latency_instrumented"], sampled)
                    self.assertEqual(len(row["latencies_ns"]), 10002 if sampled else 0)
                    self.assertEqual(len(row["timer_samples_ns"]), 10000 if sampled else 0)
                    self.assertFalse(row["checksum_in_timed_loop"])
                    if sampled:
                        q = quantiles(row["latencies_ns"])
                        self.assertLessEqual(q["p50"], q["p99"])
                        self.assertLessEqual(q["p99"], q["p999"])

    def test_reject_offline_latency_and_unknown_modes(self):
        for program, _ in self.programs:
            for args in (["--measure", "offline", "--latencies"], ["--measure", "unknown"]):
                self.assertNotEqual(self.invoke(program, args).returncode, 0)

    def test_debug_programs_explicitly_refuse_measurements(self):
        for binary in (ROOT / "build/clang-debug/book_cpp.exe", ROOT / "build/goose-debug/orderbook.exe"):
            result = self.invoke([str(binary)], ["--measure", "core"])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Release", result.stderr)


if __name__ == "__main__":
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    (ROOT / "build/benchmark-stage7-tests.json").write_text(json.dumps({"tests_run": result.testsRun,
        "failures": len(result.failures), "errors": len(result.errors)}, indent=2)+"\n", encoding="utf-8")
    raise SystemExit(0 if result.wasSuccessful() else 1)
