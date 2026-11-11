"""Check execution-owned module values through the public VM and native CLI."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


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
                assert b"register VM execution failed (6)" in vm.stderr, (name, vm.stderr)
            else:
                assert not vm.stderr, (name, vm.stderr)
        run(name + "-build", [str(binary), "build", str(source), "-o", str(executable)], 0)
        for repeat in range(2):
            native = run(f"{name}-native-{repeat}", [str(executable)], expected_exit)
            assert native.stdout == expected, (name, native.stdout, expected)
            if negative:
                assert b"XR_RUN_6002: value operation rejected" in native.stderr, (name, native.stderr)
            else:
                assert not native.stderr, (name, native.stderr)
    print(f"{len(records)} module-state commands passed")


if __name__ == "__main__":
    main()
