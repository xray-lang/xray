"""Retired object identities do not reserve ordinary nominal declaration names."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import check_structural_object_json_map_boundary as guard


class ObjectBoundaryGuard(unittest.TestCase):
    def test_nominal_record_name_and_real_retired_identities(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "spec").mkdir()
            (root / "src").mkdir()
            (root / "spec/example.xr").write_text(
                'struct Record { value: i64 }\nconst value=Record{value:42}\n', encoding='utf-8')
            self.assertFalse(any(guard.collect(root).values()))
            (root / "spec/removed.xr").write_text('const old=Type.record\n', encoding='utf-8')
            (root / "src/removed.h").write_text(
                'enum Old { AST_OBJECT_LITERAL, XR_BK_STRUCT_OBJECT, AST_SET_LITERAL };\n', encoding='utf-8')
            findings = guard.collect(root)
            self.assertEqual(len(findings['PUBLIC_RECORD_SURFACE']), 1)
            self.assertEqual(len(findings['DELETED_STRUCTURAL_OBJECT_IDENTITY']), 1)


if __name__ == '__main__':
    unittest.main()
