"""Check byte-for-byte captures and independently frozen record expectations."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).parent / "fixtures"
for name, digest in json.loads((root / "sha256.json").read_text()).items():
    assert hashlib.sha256((root / name).read_bytes()).hexdigest() == digest, name
for name, count in (("msvc", 229), ("clang", 182), ("zig", 182), ("msvc-link", 35)):
    rows = (root / (name + ".expected")).read_text(encoding="utf8").splitlines()
    assert len(rows) == count, name
    for row in rows:
        kind, offset, path = row.split("\t", 2)
        assert kind in ("0", "1", "2") and int(offset) >= 0 and path
print("four original captures and independent expectation counts verified")
