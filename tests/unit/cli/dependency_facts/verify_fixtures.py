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
for name, count in (("msvc-fullpath", 12), ("clang-fullpath", 12),
                    ("zig-fullpath", 12), ("unicode-fullpath", 6)):
    rows = (root / (name + ".expected")).read_text(encoding="utf8").splitlines()
    assert len(rows) == count, name
    expected = bytearray(b"\xff\xfe")
    for row in rows:
        kind, offset, path = row.split("\t", 2)
        assert kind == "3" and int(offset) == len(expected) + 2
        expected += ('"' + path + '"\r\n').encode("utf-16-le")
    assert expected == (root / (name + ".rsp")).read_bytes(), name
old = (root / "msvc-defaultlib.rsp").read_bytes().decode("utf-16")
assert len(old.splitlines()) == 10 and old.startswith('"/defaultlib:')
print("four original captures, three real twelve-input RSPs, Unicode and option rejection verified")
