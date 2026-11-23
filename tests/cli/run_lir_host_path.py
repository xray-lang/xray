"""Check shared host path results and filesystem effects on both product backends."""
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

    fixtures = root / "tests/fixtures/lir_cli/host_path"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    args.output.mkdir(parents=True, exist_ok=True)

    def run(command, cwd, stem):
        out, err = cwd / (stem + ".stdout"), cwd / (stem + ".stderr")
        with out.open("xb") as stdout, err.open("xb") as stderr:
            status = run_child(list(map(str, command)), cwd=cwd, env=environment,
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
        return status, out.read_bytes(), err.read_bytes()

    with tempfile.TemporaryDirectory(prefix="host-path-", dir=args.output.resolve()) as directory:
        work = Path(directory)
        for name in ("mkdir", "nul_path", "unregistered"):
            source = fixtures / (name + ".xr")
            vm_dir, native_dir = work / (name + "-vm"), work / (name + "-native")
            for cwd in (vm_dir, native_dir):
                cwd.mkdir()
                (cwd / "file").write_bytes(b"keep")
            vm = run([binary, "run", source], vm_dir, "run")
            output = native_dir / ("program.exe" if os.name == "nt" else "program")
            built = run([binary, "build", source, "-o", output], native_dir, "build")
            if name == "unregistered":
                assert vm == built, (name, vm, built)
                assert vm[0] == 1 and not vm[1] and not output.exists(), (name, vm)
                assert b"function without a scalar body is not supported by LIR lowering yet" in vm[2], vm
            else:
                assert built[0] == 0 and not built[2], (name, built)
                native = run([output], native_dir, "run")
                assert vm == native, (name, vm, native)
                if name == "mkdir":
                    assert vm == (0, b"true\ntrue\nfalse\nfalse\n", b""), vm
                    assert all((cwd / "目录").is_dir() and not (cwd / "missing").exists()
                               for cwd in (vm_dir, native_dir)), name
                else:
                    assert vm[0] == 1 and not vm[1] and b"XR_RUN_6002: value operation rejected (status 1)" in vm[2], vm
                    assert all(not (cwd / "must-not-exist").exists() for cwd in (vm_dir, native_dir)), name
            assert all((cwd / "file").read_bytes() == b"keep" for cwd in (vm_dir, native_dir)), name
            print(f"{name}: VM and native agree on output, exit and filesystem effects")


if __name__ == "__main__":
    main()
