"""Compare the CLI backends on hosted file writes and exact filesystem effects."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def check_files(directory: Path, expected: dict[str, bytes], absent: list[str]) -> None:
    for path in absent:
        assert not os.path.lexists(directory / path), (directory, "unexpected path", path)
    actual = {}
    for path in directory.rglob("*"):
        assert path.is_file() and not path.is_symlink(), (directory, "expected a regular file", path)
        actual[path.relative_to(directory).as_posix()] = path.read_bytes()
    assert actual == expected, (directory, actual, expected)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    fixtures = root / "tests/fixtures/lir_cli/host_file_write"
    cases = json.loads((fixtures / "cases.json").read_text(encoding="utf-8"))
    assert cases and set(cases) == {path.stem for path in fixtures.glob("*.xr")}
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    args.output.mkdir(parents=True, exist_ok=True)

    def run(command: list[str | Path], cwd: Path, evidence: Path) -> tuple[int, bytes, bytes]:
        out, err = evidence.with_suffix(".stdout"), evidence.with_suffix(".stderr")
        with out.open("xb") as stdout, err.open("xb") as stderr:
            status = run_child(list(map(str, command)), cwd=cwd, env=environment,
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
        return status, out.read_bytes(), err.read_bytes()

    failed = []
    with tempfile.TemporaryDirectory(prefix="host-file-write-", dir=args.output.resolve()) as directory:
        for name, case in cases.items():
            source = fixtures / (name + ".xr")
            work = Path(directory) / name
            vm_dir, native_dir = work / "vm", work / "native"
            seed = {path: bytes.fromhex(data) for path, data in case["seed_hex"].items()}
            expected = {path: bytes.fromhex(data) for path, data in case["final_hex"].items()}
            absent = case.get("absent", [])
            for cwd in vm_dir, native_dir:
                cwd.mkdir(parents=True)
                for path, data in seed.items():
                    (cwd / path).write_bytes(data)
            output = work / ("program.exe" if os.name == "nt" else "program")
            try:
                vm = run([binary, "run", source], vm_dir, work / "vm-run")
                built = run([binary, "build", source, "-o", output], native_dir, work / "native-build")
                check_files(native_dir, seed, absent)
                assert built[0] == 0 and not built[2] and output.is_file(), ("build", built, "VM", vm)
                native = run([output], native_dir, work / "native-run")
                assert vm == native == (0, case["stdout_utf8"].encode("utf-8"), b""), (vm, native)
                for cwd in vm_dir, native_dir:
                    check_files(cwd, expected, absent)
            except (AssertionError, subprocess.TimeoutExpired) as error:
                failed.append(name)
                print(f"{name}: FAIL {error}", file=sys.stderr)
            else:
                print(f"{name}: VM and native agree on output, exit and filesystem effects")

    if failed:
        raise SystemExit(f"{len(failed)} failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
