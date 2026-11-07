"""Exercise public LIR execution, ordinary native output and runtime relocation."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime-library", type=Path, action="append", required=True)
    args = parser.parse_args()
    binary, root = args.binary.resolve(), args.root.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="lir-cli-", dir=args.output.resolve()))
    fixtures = root / "tests/fixtures/lir_cli"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    records = []

    def run(name, command, expected):
        argv = list(map(str, command))
        started = time.perf_counter()
        child = subprocess.run(argv, cwd=work, env=environment, capture_output=True, timeout=30)
        record = {"name": name, "command": argv, "exit": child.returncode,
                  "seconds": time.perf_counter() - started}
        for stream, data in (("stdout", child.stdout), ("stderr", child.stderr)):
            path = work / (name + "." + stream)
            path.write_bytes(data)
            record[stream] = {"path": str(path), "sha256": hashlib.sha256(data).hexdigest()}
        records.append(record)
        (work / "result.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        assert child.returncode == expected, (name, child.returncode, child.stdout, child.stderr)
        return child

    expected = {"hello": b"42\n", "call": b"42\n", "unicode": "你好\n".encode(),
                "f64": b"3.75\n"}
    for name, stdout in expected.items():
        source = fixtures / (name + ".xr")
        vm = run(name + "-vm", [binary, "run", source], 0)
        assert vm.stdout == stdout and not vm.stderr, name
        executable = work / (name + (".exe" if os.name == "nt" else ""))
        run(name + "-build", [binary, "build", source, "-o", executable], 0)
        native = run(name + "-native", [executable], 0)
        assert (native.stdout, native.stderr) == (vm.stdout, vm.stderr), name

    source = fixtures / "panic.xr"
    vm = run("panic-vm", [binary, "run", source], 1)
    executable = work / ("panic.exe" if os.name == "nt" else "panic")
    run("panic-build", [binary, "build", source, "-o", executable], 0)
    native = run("panic-native", [executable], 1)
    assert vm.stderr and (native.stdout, native.stderr) == (vm.stdout, vm.stderr)

    generated = work / "hello.c"
    run("c-only", [binary, "build", fixtures / "hello.xr", "--c-only", "-o", generated], 0)
    assert "int main(void)" in generated.read_text(encoding="utf-8")
    sentinel = b"previous output\n"
    for flag, path in (([], work / "protected.exe"), (["--c-only"], generated)):
        path.write_bytes(sentinel)
        run("invalid-" + path.suffix[1:],
            [binary, "build", fixtures / "invalid.xr", *flag, "-o", path], 1)
        assert path.read_bytes() == sentinel

    for command in ("repl", "dap", "test"):
        refused = run("unavailable-" + command, [binary, command], 1)
        assert not refused.stdout and b"temporarily unavailable" in refused.stderr
    refused = run("unavailable-sandbox", [binary, "run", "--sandbox", fixtures / "hello.xr"], 1)
    assert not refused.stdout and b"temporarily unavailable" in refused.stderr
    probe = run("probe", [binary, "toolchain", "probe", "--json"], 0)
    assert json.loads(probe.stdout)["ready"] is True and not probe.stderr

    prefix = work / "relocated with 空格"
    (prefix / "bin").mkdir(parents=True)
    relocated = prefix / "bin" / binary.name
    shutil.copy2(binary, relocated)
    for runtime in binary.parent.glob("clang_rt.*.dll"):
        shutil.copy2(runtime, relocated.parent / runtime.name)
    absent = run("relocated-missing-runtime",
                 [relocated, "build", fixtures / "hello.xr", "-o", work / "absent.exe"], 1)
    assert b"native runtime" in absent.stderr and not (work / "absent.exe").exists()
    runtime = prefix / "lib/xray/native"
    (runtime / "include").mkdir(parents=True)
    for library in args.runtime_library:
        shutil.copy2(library, runtime / library.name)
    for header in ("xray_native_runtime.h", "xray_export.h"):
        shutil.copy2(root / "include" / header, runtime / "include" / header)
    executable = work / ("relocated.exe" if os.name == "nt" else "relocated")
    run("relocated-build", [relocated, "build", fixtures / "hello.xr", "-o", executable], 0)
    native = run("relocated-run", [executable], 0)
    assert native.stdout == b"42\n" and not native.stderr
    print(f"{len(records)} public LIR command checks passed")


if __name__ == "__main__":
    main()
