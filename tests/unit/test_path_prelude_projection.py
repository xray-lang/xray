"""The owning builtin route projects once and rejects ambiguous input."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location("path_native_generator",ROOT/"scripts/gen_native_declarations.py")
GEN=importlib.util.module_from_spec(SPEC)
import sys
sys.modules[SPEC.name]=GEN
SPEC.loader.exec_module(GEN)
class PathPreludeProjection(unittest.TestCase):
    def test_actual_route(self):
        rows=GEN.prelude_declaration_rows(ROOT)
        self.assertEqual([row for row in rows if row[0]=="Path"],[("Path","path")])
    def test_duplicate_route(self):
        source='XR_BUILTIN_PRELUDE_DECLARATION("Path", "path")\n'*2
        with patch.object(Path,"read_text",return_value=source):
            with self.assertRaisesRegex(ValueError,"duplicate builtin"):
                GEN.prelude_declaration_rows(ROOT)
    def test_missing_route(self):
        with patch.object(Path,"read_text",return_value=''):
            with self.assertRaisesRegex(ValueError,"requires one builtin"):
                GEN.prelude_declaration_rows(ROOT)
if __name__=="__main__":unittest.main()
