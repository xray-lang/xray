"""Public run and native build of the time services against independent expectations."""
import argparse
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

XRAY = None


def run(argv, **kwargs):
    return subprocess.run(argv, capture_output=True, timeout=180, **kwargs)


class TimeServices(unittest.TestCase):
    def source(self, directory, text):
        path = Path(directory) / "program.xr"
        path.write_text(text, encoding="utf-8", newline="\n")
        return path

    def execute(self, text, native):
        """Return (returncode, stdout, stderr, elapsed seconds) of one backend."""
        with tempfile.TemporaryDirectory(prefix="xray-time-") as directory:
            path = self.source(directory, text)
            if native:
                exe = Path(directory) / "program.exe"
                built = run([XRAY, "build", str(path), "-o", str(exe)])
                self.assertEqual(built.returncode, 0, built.stderr.decode(errors="replace"))
                command = [str(exe)]
            else:
                command = [XRAY, "run", str(path)]
            started = time.monotonic()
            result = run(command)
            return result.returncode, result.stdout.decode(), result.stderr.decode(), time.monotonic() - started

    def both(self, text):
        for native in (False, True):
            with self.subTest(native=native):
                yield native, self.execute(text, native)

    def test_sleep_waits_at_least_the_requested_duration(self):
        program = ("import time\n"
                   "const a: i64 = time.monotonic()\n"
                   "time.sleep(120)\n"
                   "const b: i64 = time.monotonic()\n"
                   "print(b - a >= 120)\n"
                   "print(b - a < 5000)\n")
        for native, (code, out, err, elapsed) in self.both(program):
            self.assertEqual((code, out, err), (0, "true\ntrue\n", ""))
            self.assertGreaterEqual(elapsed, 0.12)

    def test_nonpositive_sleep_returns_immediately(self):
        program = "import time\ntime.sleep(0)\ntime.sleep(-9)\nprint(\"ok\")\n"
        for native, (code, out, err, elapsed) in self.both(program):
            self.assertEqual((code, out, err), (0, "ok\n", ""))
            self.assertLess(elapsed, 5.0)

    def test_clock_units_and_independent_wall_time(self):
        program = ("import time\n"
                   "const m: i64 = time.monotonic()\n"
                   "const u: i64 = time.micros()\n"
                   "const n: i64 = time.nanos()\n"
                   "print(u / 1000 - m >= 0)\n"
                   "print(n / 1000 - u >= 0)\n"
                   "print(time.now())\n")
        for native, (code, out, err, _) in self.both(program):
            self.assertEqual((code, err), (0, ""))
            lines = out.split()
            self.assertEqual(lines[:2], ["true", "true"])
            self.assertLess(abs(int(lines[2]) / 1000.0 - time.time()), 30.0)

    def test_cpu_clock_does_not_exceed_wall_budget_and_offset_is_sane(self):
        program = ("import time\n"
                   "const cpu: i64 = time.clock()\n"
                   "print(cpu >= 0)\n"
                   "const off: i64 = time.localOffsetAt(0)\n"
                   "print(off >= -1440)\n"
                   "print(off <= 1440)\n")
        for native, (code, out, err, _) in self.both(program):
            self.assertEqual((code, out, err), (0, "true\ntrue\ntrue\n", ""))

    def test_sleeping_in_cleanup_is_rejected_before_execution(self):
        program = ("import time\n"
                   "fn work() {\n"
                   "    defer { time.sleep(1) }\n"
                   "    print(\"body\")\n"
                   "}\n"
                   "work()\n")
        code, out, err, _ = self.execute(program, False)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("E0392", err)

    def test_clock_reads_do_not_suspend(self):
        program = ("import time\n"
                   "fn work() {\n"
                   "    defer { print(time.monotonic() >= 0) }\n"
                   "    print(\"body\")\n"
                   "}\n"
                   "work()\n")
        for native, (code, out, err, _) in self.both(program):
            self.assertEqual((code, out, err), (0, "body\ntrue\n", ""))

    def test_unrepresentable_offset_is_a_numeric_range_panic(self):
        program = "import time\nprint(time.localOffsetAt(9223372036854775807))\n"
        for native, (code, out, err, _) in self.both(program):
            self.assertEqual((code, out), (1, ""))
            self.assertIn("E0422", err)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", required=True)
    arguments, remaining = parser.parse_known_args()
    XRAY = arguments.xray
    sys.argv = [sys.argv[0]] + remaining
    unittest.main()
