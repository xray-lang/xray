"""Language surface fixtures run on both back ends against independent expectations.

Every `<name>.xr` under tests/fixtures/xir_language is paired with exactly one
expectation file:
  <name>.out     exact stdout; the program exits 0 under `xray run` and as a
                 natively built executable
  <name>.reject  one line that must appear in the `xray check` diagnostic; the
                 program is rejected with a nonzero status and builds nothing
The expectations are written from the language rules, not captured from a run.
"""
import argparse
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

XRAY = None
FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "xir_language"


def run(argv, **kwargs):
    return subprocess.run(argv, capture_output=True, timeout=240, **kwargs)


class LanguageSurface(unittest.TestCase):
    def check_ok(self, source, expected):
        result = run([XRAY, "run", str(source)])
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertEqual(result.stdout.decode(), expected)
        self.assertEqual(result.stderr.decode(), "")
        with tempfile.TemporaryDirectory(prefix="xray-lang-") as directory:
            exe = Path(directory) / "program.exe"
            built = run([XRAY, "build", str(source), "-o", str(exe)])
            self.assertEqual(built.returncode, 0, built.stderr.decode(errors="replace"))
            native = run([str(exe)])
            self.assertEqual(native.returncode, 0, native.stderr.decode(errors="replace"))
            self.assertEqual(native.stdout.decode(), expected)

    def check_reject(self, source, fragment):
        result = run([XRAY, "check", str(source)])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(fragment, result.stderr.decode(errors="replace"))


def add_cases():
    for source in sorted(FIXTURES.glob("*.xr")):
        out = source.with_suffix(".out")
        reject = source.with_suffix(".reject")
        if out.is_file() == reject.is_file():
            raise SystemExit(f"{source.name}: exactly one of .out and .reject is required")
        if out.is_file():
            expected = out.read_text(encoding="utf-8").replace("\r\n", "\n")
            setattr(LanguageSurface, f"test_{source.stem}",
                    lambda self, s=source, e=expected: self.check_ok(s, e))
        else:
            fragment = reject.read_text(encoding="utf-8").strip()
            setattr(LanguageSurface, f"test_{source.stem}",
                    lambda self, s=source, f=fragment: self.check_reject(s, f))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", required=True)
    args, rest = parser.parse_known_args()
    XRAY = str(Path(args.xray).resolve())
    add_cases()
    unittest.main(argv=[sys.argv[0]] + rest)
