from pathlib import Path
import os
import subprocess
import sys
import tempfile


binary, stdlib = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="xray-lsp-named-") as temporary:
    root = Path(temporary)
    (root / "xray.toml").write_text(
        '[project]\nname="lsp-named"\nmain="root.xr"\n', encoding="utf-8")
    source = root / "root.xr"
    result = subprocess.run(
        [str(binary.resolve()), source.as_uri()], capture_output=True, timeout=30,
        env={**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())})
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    assert result.stdout.endswith(b"PASS physical0/0\r\n") or result.stdout.endswith(b"PASS physical0/0\n")
    assert not source.exists()
    print(result.stdout.decode("utf-8"), end="")
