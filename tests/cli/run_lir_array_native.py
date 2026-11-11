"""Compile authentic Array sources and execute their ordinary C projections."""
from __future__ import annotations

import argparse
import hashlib
import json
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
    parser.add_argument("--compiler", action="append", default=[])
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="array-native-", dir=args.output.resolve()))
    fixtures = root / "tests/fixtures/lir_cli/array_lower"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    records = []
    failures = []

    def run(name, command, expected_exit):
        command = list(map(str, command))
        paths = {stream: work / (name + "." + stream) for stream in ("stdout", "stderr")}
        with paths["stdout"].open("xb") as out, paths["stderr"].open("xb") as err:
            status = run_child(command, cwd=work, env=environment, stdin=subprocess.DEVNULL,
                               stdout=out, stderr=err, timeout=30)
        row = {"name": name, "argv": command, "exit": status, "expected_exit": expected_exit,
               "state": "TERMINAL_DRAINED"}
        for stream, path in paths.items():
            row[stream] = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        records.append(row)
        (work / "result.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        if status != expected_exit:
            failures.append((name, status, paths["stderr"].read_text(encoding="utf-8")))
        return status, paths["stdout"].read_bytes(), paths["stderr"].read_bytes()

    for name in ("snapshots", "values", "strings", "imports", "conditional", "large", "local_loop", "loop_temporary",
                 "panic_bounds", "panic_unpublished"):
        source = fixtures / (name + ".xr")
        status, _, _ = run(name + "-check", [binary, "check", source], 0)
        if status:
            continue
        generated = work / (name + ".c")
        status, _, _ = run(name + "-c", [binary, "build", "-c", source, "-o", generated], 0)
        if status:
            continue
        assert "({" not in generated.read_text(encoding="utf-8")
        if name == "large":
            assert "UINT32_C(300)" in generated.read_text(encoding="utf-8")
        for ordinal, compiler in enumerate(args.compiler or [None]):
            executable = work / (name + f"-{ordinal}" + (".exe" if os.name == "nt" else ""))
            command = [binary, "build", source, "-o", executable]
            if compiler:
                command += ["--cc", compiler]
            status, _, _ = run(name + f"-build-{ordinal}", command, 0)
            if status:
                continue
            panic = name.startswith("panic_")
            for repeat in range(2):
                _, out, err = run(name + f"-native-{ordinal}-{repeat}", [executable], 1 if panic else 0)
                if panic:
                    if out or b"XR_RUN_6002: value operation rejected" not in err or source.name.encode() not in err:
                        failures.append((name, "runtime diagnostic", out, err))
                elif out != source.with_suffix(".out").read_bytes() or err:
                    failures.append((name, "output", out, err))
    for name in ("reject_bool", "reject_nested"):
        source = fixtures / (name + ".xr")
        run(name + "-check", [binary, "check", source], 0)
        _, out, err = run(name + "-c", [binary, "build", "-c", source, "-o", work / (name + ".c")], 1)
        if out or b"module slot payload type is not supported" not in err:
            failures.append((name, "unsupported payload", out, err))
    assert not failures, failures
    print(f"{len(records)} Array Source/native commands passed")


if __name__ == "__main__":
    main()
