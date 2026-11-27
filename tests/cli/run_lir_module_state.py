"""Check execution-owned module values through the public VM and native CLI."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.artifacts import complete_native_artifacts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, root = args.binary.resolve(), args.root.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="module-state-", dir=args.output.resolve()))
    fixtures = root / "tests/fixtures/lir_cli"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    records = []

    def run(name: str, argv: list[str], expected_exit: int) -> subprocess.CompletedProcess:
        result = subprocess.run(argv, env=environment, cwd=work, capture_output=True, timeout=30)
        record = {"name": name, "argv": argv, "exit": result.returncode}
        for key in ("stdout", "stderr"):
            data = getattr(result, key)
            path = work / (name + "." + key)
            path.write_bytes(data)
            record[key] = {"path": str(path), "sha256": hashlib.sha256(data).hexdigest()}
        records.append(record)
        (work / "result.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        assert result.returncode == expected_exit, (name, result.returncode, result.stdout, result.stderr)
        return result

    for name in ("module_state_mixed", "module_state_branch", "module_state_initializers",
                 "module_state_acyclic_class", "module_state_class_array", "module_state_inherited_class",
                 "module_state_class_nested_owners",
                 "module_state_unpublished", "module_state_unpublished_cycle",
                 "module_state_unpublished_const"):
        source = fixtures / (name + ".xr")
        negative = name.startswith("module_state_unpublished")
        expected_exit = 1 if negative else 0
        expected = b"" if negative else source.with_suffix(".out").read_bytes()
        executable = work / (name + (".exe" if os.name == "nt" else ""))
        for repeat in range(2):
            vm = run(f"{name}-vm-{repeat}", [str(binary), "run", str(source)], expected_exit)
            assert vm.stdout == expected, (name, vm.stdout, expected)
            if negative:
                assert b"XR_RUN_6002: value operation rejected" in vm.stderr, (name, vm.stderr)
            else:
                assert not vm.stderr, (name, vm.stderr)
        run(name + "-build", [str(binary), "build", str(source), "-o", str(executable)], 0)
        for repeat in range(2):
            native = run(f"{name}-native-{repeat}", [str(executable)], expected_exit)
            assert native.stdout == expected, (name, native.stdout, expected)
            if negative:
                assert b"XR_RUN_6002: value operation rejected" in native.stderr, (name, native.stderr)
                assert native.stderr == vm.stderr, (name, vm.stderr, native.stderr)
            else:
                assert not native.stderr, (name, native.stderr)

    order_fixtures = fixtures / "module_init_order"
    source = order_fixtures / "positive" / "main.xr"
    expected = source.with_suffix(".out").read_text(encoding="ascii").encode("ascii")
    executable = work / ("module_order.exe" if os.name == "nt" else "module_order")
    for mode, argv in (
        ("vm", [str(binary), "run", str(source)]),
        ("build", [str(binary), "build", str(source), "-o", str(executable)]),
        ("native", [str(executable)]),
    ):
        result = run("module_order-" + mode, argv, 0)
        assert not result.stderr, (mode, result.stderr)
        if mode != "build":
            assert result.stdout == expected, (mode, result.stdout, expected)

    source = order_fixtures / "cycle" / "main.xr"
    executable = work / ("module_cycle.exe" if os.name == "nt" else "module_cycle")
    for mode in ("check", "run", "build"):
        argv = [str(binary), mode, str(source)]
        if mode == "build":
            argv += ["-o", str(executable)]
        refused = run("module_cycle-" + mode, argv, 1)
        assert refused.stdout == b"" and b"module graph ordering failed" in refused.stderr, (
            mode, refused.stdout, refused.stderr
        )
        assert not executable.exists()

    complete_native_artifacts(work)
    print(f"{len(records)} module-state commands passed")


if __name__ == "__main__":
    main()
