"""Actual shared first-error state at all preference entry boundaries."""
from pathlib import Path
import subprocess
import sys
import tempfile

exe, parent = map(lambda value: Path(value).resolve(), sys.argv[1:3])
parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="state-中-", dir=parent) as directory:
    path = Path(directory) / "toolchains.toml"
    original = b'schema=1\n[native]\nprovider="msvc"\ncompiler="before.exe"\n'
    path.write_bytes(original)
    result = subprocess.run([str(exe), str(path), "state"], capture_output=True, timeout=30)
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    assert result.returncode == 0
    assert path.read_bytes() == original
    assert set(Path(directory).iterdir()) == {path}
