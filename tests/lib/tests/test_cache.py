"""Cache-key tests. The load-bearing one: the key must not miss a dependency.

A key that ignores a real input reports a hit for a state it never established.
These assertions pin the two properties that prevent that -- closure coverage
and change-sensitivity -- so a future edit cannot quietly reintroduce the glob
that dropped 54 headers.
"""

import unittest
from pathlib import Path

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import cache  # noqa: E402


class IncludeClosureTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.mkdtemp(prefix="xt_cache.")
        self.root = Path(self.tmp)

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
        return p

    def test_closure_reaches_headers_outside_the_root_dir(self):
        # a.h includes a sibling and a header in another directory; the closure
        # must contain both, which a glob of a.h's directory would miss.
        self._write("src/aot/a.h", '#include "b.h"\n#include "../shared/c.h"\n')
        self._write("src/aot/b.h", "int b;\n")
        self._write("src/shared/c.h", "int c;\n")
        closure = cache.include_closure(self.root / "src/aot/a.h", self.root)
        names = {p.name for p in closure}
        self.assertEqual(names, {"a.h", "b.h", "c.h"})

    def test_closure_ignores_system_includes(self):
        self._write("src/aot/a.h", '#include <stdio.h>\n#include "b.h"\n')
        self._write("src/aot/b.h", "int b;\n")
        closure = cache.include_closure(self.root / "src/aot/a.h", self.root)
        self.assertEqual({p.name for p in closure}, {"a.h", "b.h"})

    def test_closure_survives_include_cycles(self):
        self._write("src/aot/a.h", '#include "b.h"\n')
        self._write("src/aot/b.h", '#include "a.h"\n')
        closure = cache.include_closure(self.root / "src/aot/a.h", self.root)
        self.assertEqual({p.name for p in closure}, {"a.h", "b.h"})

    def test_missing_file_is_a_stable_marker_not_a_skip(self):
        # An absent file must contribute to the key, so its appearance changes
        # the key rather than being silently ignored.
        self.assertEqual(cache.file_digest(self.root / "nope.h"), cache.MISSING)

    def test_key_changes_when_a_closure_header_changes(self):
        self._write("src/aot/xrt.h", '#include "dep.h"\n')
        self._write("src/aot/dep.h", "int v = 1;\n")
        self._write("src/ir/xi_method_sym.def", "SYM(x)\n")
        self._write("build/xray", "binary")
        xray = self.root / "build/xray"
        before = cache.toolchain_key(xray, self.root)
        # Change only a transitively-included header, nothing in src/aot's glob
        # would have to change for this to matter.
        self._write("src/aot/dep.h", "int v = 2;\n")
        after = cache.toolchain_key(xray, self.root)
        self.assertNotEqual(before, after)

    def test_key_stable_when_nothing_changes(self):
        self._write("src/aot/xrt.h", "int v;\n")
        self._write("build/xray", "binary")
        xray = self.root / "build/xray"
        self.assertEqual(
            cache.toolchain_key(xray, self.root),
            cache.toolchain_key(xray, self.root),
        )


class DirLockTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.mkdtemp(prefix="xt_lock.")
        self.root = Path(self.tmp)

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_lock_is_exclusive(self):
        lock_path = self.root / "the.lock"
        first = cache.DirLock(lock_path)
        self.assertTrue(first.acquire())
        try:
            import os

            os.environ["XRAY_TEST_LOCK_TIMEOUT"] = "0"
            second = cache.DirLock(lock_path)
            self.assertFalse(second.acquire())
        finally:
            import os

            os.environ.pop("XRAY_TEST_LOCK_TIMEOUT", None)
            first.release()

    def test_lock_released_on_context_exit(self):
        lock_path = self.root / "ctx.lock"
        with cache.DirLock(lock_path):
            self.assertTrue(lock_path.is_dir())
        self.assertFalse(lock_path.exists())


if __name__ == "__main__":
    unittest.main()
