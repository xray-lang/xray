"""Check String.slice runtime output and panic diagnostics on both backends."""
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
    fixtures = root / "tests/fixtures/lir_cli/string_slice"
    args.output.mkdir(parents=True, exist_ok=True)

    sliced = (
        "é中\n中😀\n\0\n\u0301\n\n\n"
        "receiver\nstart\nend\n中\n"
        "中😀\nx\n中😀\n"
    ).encode("utf-8")
    range_failure = (1, b"before\n", b"[Uncaught Panic] E0433: string slice range is invalid\n")
    cases = {
        "slices": (0, sliced, b""),
        "negative_start": range_failure,
        "reversed_range": range_failure,
        "past_end": range_failure,
        "omitted_end_past_end": range_failure,
    }

    def run(command, cwd, stage):
        output, errors = cwd / (stage + ".stdout"), cwd / (stage + ".stderr")
        with output.open("xb") as stdout, errors.open("xb") as stderr:
            status = run_child(list(map(str, command)), cwd=cwd, env=environment,
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
        return status, output.read_bytes(), errors.read_bytes()

    with tempfile.TemporaryDirectory(prefix="string-slice-", dir=args.output.resolve()) as directory:
        for name, expected in cases.items():
            cwd = Path(directory) / name
            cwd.mkdir()
            source = fixtures / (name + ".xr")
            executable = cwd / ("program.exe" if os.name == "nt" else "program")
            built = run([binary, "build", source, "-o", executable], cwd, "build")
            assert built[0] == 0 and not built[2] and executable.is_file(), (name, "build", built)
            vm = run([binary, "run", source], cwd, "vm")
            native = run([executable], cwd, "native")
            assert vm == native == expected, (name, vm, native, expected)
            print(f"{name}: VM and native agree on output and exit")


if __name__ == "__main__":
    main()
