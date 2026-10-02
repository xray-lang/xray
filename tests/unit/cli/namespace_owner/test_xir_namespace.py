"""Create isolated namespace shapes and run real NTFS mutation gates."""
from pathlib import Path
import subprocess
import sys
import tempfile
import os

with tempfile.TemporaryDirectory(prefix="xray-namespace-") as temporary:
    root = Path(temporary)
    for name in ("tree", "direct", "readonly", "overlap"):
        (root / name / "child/deep").mkdir(parents=True)
        (root / name / "existing.h").write_bytes(b"independent header bytes\n")
    (root / "missing").mkdir()
    (root / "race").mkdir()
    (root / "matrix/child").mkdir(parents=True)
    # This is fixture preparation before any guard exists. Directory reads can
    # themselves invalidate a parent's R oplock on this NTFS configuration.
    # A new owner must still arm, enumerate and check the entire interval.
    for directory, _, names in os.walk(root):
        for name in names:
            (Path(directory) / name).read_bytes()
    print("fixture enumeration/read preparation completed before the guarded interval", flush=True)
    result = subprocess.run([sys.argv[1], str(root)], capture_output=True, timeout=170)
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    assert result.returncode == 0, result.returncode
    assert not (root / "tree/child/deep/toggle.h").exists()
    assert not (root / "missing/absent").exists()
    assert (root / "tree/existing.h").read_bytes() == b"independent header bytes\n"
print("explicit NTFS roots only; read-only enumeration, sticky changes and owned completion PASS")
