"""Retained cleanup owner and already-published configuration survive retry."""
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

exe, parent = map(lambda value: Path(value).resolve(), sys.argv[1:3])
parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="cleanup-中-", dir=parent) as directory:
    config = Path(directory) / "toolchains.toml"
    result = subprocess.run([str(exe), str(config), "cleanup"], capture_output=True, timeout=30)
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    assert result.returncode == 0
    assert tomllib.loads(config.read_text(encoding="utf-8"))["native"] == {
        "provider": "msvc", "compiler": "retained.exe"}
    assert set(Path(directory).iterdir()) == {config, Path(str(config) + ".lock")}
