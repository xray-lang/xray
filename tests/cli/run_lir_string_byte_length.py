"""Check complete String byte lengths and preserve Source view admission."""
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
    fixtures = root / "tests/fixtures/lir_cli/string_byte_length"
    args.output.mkdir(parents=True, exist_ok=True)
    positives = {"byte_length": b"0\n9\n4\n9\n8\n5\n", "other_method": b"9\n2\n", "array_view": b"10\n"}
    negatives = {
        "active_view_write": b"during view lifetime normalization",
        "invalid_arity": b"bytes requires its exact lowered readonly String declaration",
    }

    def run(command, cwd, stage):
        output, errors = cwd / (stage + ".stdout"), cwd / (stage + ".stderr")
        with output.open("xb") as stdout, errors.open("xb") as stderr:
            status = run_child(list(map(str, command)), cwd=cwd, env=environment,
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
        return status, output.read_bytes(), errors.read_bytes()

    with tempfile.TemporaryDirectory(prefix="string-byte-length-", dir=args.output.resolve()) as directory:
        for name in (*positives, *negatives):
            cwd = Path(directory) / name
            cwd.mkdir()
            source = fixtures / (name + ".xr")
            binary_output = cwd / ("program.exe" if os.name == "nt" else "program")
            vm = run([binary, "run", source], cwd, "vm")
            built = run([binary, "build", source, "-o", binary_output], cwd, "build")
            if name in negatives:
                assert vm == built and vm[0] == 1 and not vm[1] and not binary_output.exists(), (name, vm, built)
                assert negatives[name] in vm[2], (name, vm)
            else:
                assert built[0] == 0 and not built[2] and binary_output.is_file(), (name, built)
                native = run([binary_output], cwd, "native")
                assert vm == native == (0, positives[name], b""), (name, vm, native)
                if name == "byte_length":
                    c_source = cwd / "program.c"
                    generated = run([binary, "build", "--c-only", source, "-o", c_source], cwd, "cgen")
                    assert generated[0] == 0 and not generated[2], (name, generated)
                    c = c_source.read_bytes()
                    assert b"(int64_t)value->length" in c and b"xrt_array_slice_" not in c
            print(f"{name}: VM and native agree on output and exit")


if __name__ == "__main__":
    main()
