"""Reject ambiguous or incomplete canonical Unicode projections before embedding."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
import gen_regex_unicode_projection as projection


class UnicodeProjection(unittest.TestCase):
    def test_complete_exact_region(self):
        properties, tables, aliases, _ = projection.canonical(ROOT)
        self.assertEqual((len(properties), len(aliases), sum(map(len, tables.values()))), (19, 28, 113))
        self.assertEqual((properties["L"], properties["Han"], properties["Any"]), (1, 10, 19))
        self.assertEqual(projection.render(ROOT), (ROOT / "stdlib/regex/regex.xr").read_text(encoding="utf-8"))

    def test_same_profile_across_line_endings(self):
        with tempfile.TemporaryDirectory(prefix="regex-unicode-profile-") as directory:
            root = Path(directory)
            for name in ("xunicode.h", "xunicode.c"):
                path = root / "src/base" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                text = (ROOT / "src/base" / name).read_text(encoding="utf-8")
                path.write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
            self.assertEqual(projection.canonical(root), projection.canonical(ROOT))

    def test_fail_closed_data_mutations(self):
        mutations = [
            ("xunicode.c", "0x0061, 0x007A", "0x005A, 0x007A"),
            ("xunicode.c", '{"Letter", XR_UP_L}', '{"l", XR_UP_L}'),
            ("xunicode.c", '{NULL, XR_UP_INVALID}', '{NULL, XR_UP_L}'),
            ("xunicode.c", "ranges_Any[]", "ranges_Missing[]"),
            ("xunicode.h", "XR_UP_COUNT", "XR_UP_Extra, XR_UP_COUNT"),
            ("xunicode.h", "XR_UP_L,", "XR_UP_L = 7,"),
        ]
        for name, old, new in mutations:
            with self.subTest(mutation=new), tempfile.TemporaryDirectory(prefix="regex-unicode-invalid-") as directory:
                root = Path(directory)
                for basename in ("xunicode.h", "xunicode.c"):
                    path = root / "src/base" / basename
                    path.parent.mkdir(parents=True, exist_ok=True)
                    text = (ROOT / "src/base" / basename).read_text(encoding="utf-8")
                    if basename == name:
                        self.assertIn(old, text)
                        text = text.replace(old, new, 1)
                    path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    projection.canonical(root)


if __name__ == "__main__":
    unittest.main()
