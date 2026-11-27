"""Verify an embedded dependency through VM and actual native execution."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    binary, repository = (Path(arg).resolve(strict=True) for arg in sys.argv[1:])
    fixtures = repository / "tests/fixtures/lir_cli/embedded_private_stdlib"
    with tempfile.TemporaryDirectory(prefix="xray-embedded-private-") as directory:
        root = Path(directory)

        def run(directory, argv, expected=0):
            result = subprocess.run(
                argv, cwd=root, env=dict(os.environ, XRAY_STDLIB_PATH=str(directory), NO_COLOR="1"),
                stdin=subprocess.DEVNULL, capture_output=True, timeout=30,
            )
            assert result.returncode == expected, (argv, result.stdout, result.stderr)
            return result.stdout, result.stderr

        empty = root / "empty"
        empty.mkdir()
        disk = root / "disk"
        override = disk / "path/_lexical.xr"
        override.parent.mkdir(parents=True)
        original = (repository / "stdlib/path/_lexical.xr").read_bytes()
        header = b"export fn basename(raw: string, windows: bool) -> string {"
        assert original.count(header) == 1
        override.write_bytes(original.replace(header, header + b'\n    if (raw=="dir/embedded.xr") { return "disk.xr" }'))
        fixture = fixtures / "path.xr"
        suffix = ".exe" if os.name == "nt" else ""
        for label, stdlib, expected in (("embedded", empty, b"embedded.xr\n"), ("disk", disk, b"disk.xr\n")):
            native = root / ("program-" + label + suffix)
            vm = run(stdlib, [binary, "run", fixture])
            run(stdlib, [binary, "build", fixture, "-o", native])
            assert run(stdlib, [native]) == vm == (expected, b"")
        missing = fixtures / "missing.xr"
        rejected = root / ("rejected" + suffix)
        vm = run(empty, [binary, "run", missing], 1)
        assert run(empty, [binary, "build", missing, "-o", rejected], 1) == vm
        assert vm == (b"", b"error: module not found\n") and not rejected.exists()


if __name__ == "__main__":
    main()
