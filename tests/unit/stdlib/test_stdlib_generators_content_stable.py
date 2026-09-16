import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


STDLIBGEN = load_module("xray_stdlibgen", ROOT / "tools" / "stdlibgen" / "stdlibgen.py")
ANALYZERGEN = load_module("xray_gen_stdlib_types", ROOT / "scripts" / "gen_stdlib_types.py")


class ContentStableGeneratorTests(unittest.TestCase):
    def assert_content_stable(self, writer) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "generated.inc"
            self.assertTrue(writer(output, "first\n"))
            fixed_time = 1_700_000_000_000_000_000
            os.utime(output, ns=(fixed_time, fixed_time))

            self.assertFalse(writer(output, "first\n"))
            self.assertEqual(output.stat().st_mtime_ns, fixed_time)

            self.assertTrue(writer(output, "second\n"))
            self.assertEqual(output.read_text(encoding="utf-8"), "second\n")
            self.assertNotEqual(output.stat().st_mtime_ns, fixed_time)

    def test_stdlib_metadata_writer_preserves_unchanged_mtime(self) -> None:
        self.assert_content_stable(STDLIBGEN.write_if_changed)

    def test_analyzer_metadata_writer_preserves_unchanged_mtime(self) -> None:
        self.assert_content_stable(ANALYZERGEN.write_if_changed)


if __name__ == "__main__":
    unittest.main()
