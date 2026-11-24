"""Check rune search, evaluation order and bounds on both execution paths."""
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
    fixtures = root / "tests/fixtures/lir_cli/string_index_of"
    args.output.mkdir(parents=True, exist_ok=True)
    range_panic = b"[Uncaught Panic] E0422: numeric conversion is out of range\n"
    cases = {
        "searches": (0, b"2\n5\n3\n4\n6\n-1\n7\n8\n-1\n2\n0\n-1\n"
                         b"receiver\nneedle\nstart\n1\nchanged receiver\nchanged needle\nneedle\nstart\n1\n", b""),
        "negative_start": (1, b"receiver\nneedle\nstart\n", range_panic),
        "overflow_start": (1, b"before\n", range_panic),
    }

    def run(command, cwd, stage):
        output, errors = cwd / (stage + ".stdout"), cwd / (stage + ".stderr")
        with output.open("xb") as stdout, errors.open("xb") as stderr:
            status = run_child(list(map(str, command)), cwd=cwd, env=environment,
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
        return status, output.read_bytes(), errors.read_bytes()

    with tempfile.TemporaryDirectory(prefix="string-index-of-", dir=args.output.resolve()) as directory:
        for name, expected in cases.items():
            cwd = Path(directory) / name
            cwd.mkdir()
            source = fixtures / (name + ".xr")
            executable = cwd / ("program.exe" if os.name == "nt" else "program")
            if name == "searches":
                generated = cwd / "program.c"
                c = run([binary, "build", "-c", source, "-o", generated], cwd, "c")
                assert c[0] == 0 and not c[2], (name, "generated C", c)
                assert b"xrt_native_string_index_of(" in generated.read_bytes()
            built = run([binary, "build", source, "-o", executable], cwd, "build")
            assert built[0] == 0 and not built[2] and executable.is_file(), (name, "build", built)
            vm = run([binary, "run", source], cwd, "vm")
            native = run([executable], cwd, "native")
            assert vm == native == expected, (name, vm, native, expected)
            print(f"{name}: VM and native agree on output and exit")

if __name__ == "__main__":
    main()
