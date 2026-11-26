"""Compare checked directory leaves on VM and native using a private io overlay."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, root, output = args.binary.resolve(), args.root.resolve(), args.output.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    fixtures = root / "tests/fixtures/lir_cli/host_directory"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="host-directory-", dir=output) as directory:
        work = Path(directory)
        stdlib = work / "stdlib"
        shutil.copytree(root / "stdlib", stdlib)
        with (stdlib / "io/io.xr").open("ab") as source:
            source.write(b"\n" + (fixtures / "wrappers.xr").read_bytes())
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR="1")

        def run(command, cwd, label):
            out, err = cwd / (label + ".stdout"), cwd / (label + ".stderr")
            with out.open("xb") as stdout, err.open("xb") as stderr:
                status = run_child(list(map(str, command)), cwd=cwd, env=env,
                                   stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
            return status, out.read_bytes(), err.read_bytes()

        name = "目录项.txt"
        successes = {"roundtrip": f"{name}\ntrue\n".encode(), "same_spelling": b"true\n"}
        faults = ("invalid_close", "invalid_next", "stale_next")
        for case in (*successes, *faults, "private_leaf"):
            vm_dir, native_dir = work / (case + "-vm"), work / (case + "-native")
            for cwd in (vm_dir, native_dir):
                (cwd / "one").mkdir(parents=True)
                (cwd / "one" / name).write_bytes(b"entry")
                (cwd / "empty").mkdir()
            source = fixtures / (case + ".xr")
            executable = native_dir / ("program.exe" if os.name == "nt" else "program")
            vm = run([binary, "run", source], vm_dir, "run")
            built = run([binary, "build", "--native", "-o", executable, source], native_dir, "build")
            if case == "private_leaf":
                assert vm == built and vm[0] == 1 and not vm[1] and not executable.exists(), (case, vm, built)
                assert b"import requires an exported declaration" in vm[2], (case, vm)
            else:
                assert built[0] == 0 and not built[2] and executable.is_file(), (case, built)
                native = run([executable], native_dir, "run")
                assert vm == native, (case, vm, native)
                if case in successes:
                    assert vm == (0, successes[case], b""), (case, vm)
                else:
                    assert vm[0] == 1 and not vm[1], (case, vm)
                    assert b"XR_RUN_6002: value operation rejected (status 1)" in vm[2], (case, vm)
            print(f"{case}: VM and native agree on the directory contract")


if __name__ == "__main__":
    main()
