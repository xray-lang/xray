"""Check the governed platform value and ordinary same-named calls."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    fixtures = root / "tests/fixtures/lir_cli/target_platform"
    args.output.mkdir(parents=True, exist_ok=True)

    def run(command, cwd, stem):
        out, err = cwd / (stem + ".stdout"), cwd / (stem + ".stderr")
        with out.open("xb") as stdout, err.open("xb") as stderr:
            status = run_child(list(map(str, command)), cwd=cwd, env=environment,
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
        return status, out.read_bytes(), err.read_bytes()

    platform_name = {"win32": b"windows", "linux": b"linux", "darwin": b"darwin"}[sys.platform]
    windows_fact = b"true\n" if platform_name == b"windows" else b"false\n"
    expected = (0, platform_name + b"\nuser platform\nuser __platform\n" + windows_fact * 2, b"")
    with tempfile.TemporaryDirectory(prefix="target-platform-", dir=args.output.resolve()) as directory:
        work = Path(directory)
        vm_dir, native_dir = work / "vm", work / "native"
        vm_dir.mkdir()
        native_dir.mkdir()
        source = fixtures / "platform.xr"
        output = native_dir / ("program.exe" if os.name == "nt" else "program")
        vm = run([binary, "run", source], vm_dir, "run")
        built = run([binary, "build", source, "-o", output], native_dir, "build")
        assert built == (0, f"Built: {output}\n".encode(), b""), built
        native = run([output], native_dir, "run")
        assert vm == native == expected, (vm, native, expected)

        source = fixtures / "private_platform.xr"
        vm = run([binary, "run", source], vm_dir, "private")
        rejected = native_dir / ("rejected.exe" if os.name == "nt" else "rejected")
        native = run([binary, "build", source, "-o", rejected], native_dir, "private")
        diagnostic = f"{source}:1:1: error: import requires an exported declaration\n".encode()
        assert vm == native == (1, b"", diagnostic), (vm, native, diagnostic)
        assert not rejected.exists(), rejected

    print(f"os.platform(), os.sep(), os.eol() and same-named calls agree on {sys.platform}")
    print("The private platform declaration is rejected on both backends")


if __name__ == "__main__":
    main()
