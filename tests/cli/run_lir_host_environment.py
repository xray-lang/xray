"""Check controlled environment lookups through the register VM and native executable."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest import proc
from xraytest.artifacts import complete_native_artifacts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, help="Absolute path to the host C compiler")
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    if args.compiler and (not args.compiler.is_absolute() or not args.compiler.is_file()):
        parser.error("--compiler must name an existing absolute compiler path")

    root, binary = args.root.resolve(), args.binary.resolve()
    fixtures = root / "tests/fixtures/lir_cli/host_environment"
    cases = json.loads((fixtures / "cases.json").read_text(encoding="utf-8"))
    if not cases or {case["source"] for case in cases.values()} != {path.name for path in fixtures.glob("*.xr")}:
        parser.error("environment fixtures and cases.json must match exactly")
    unknown = set(args.case) - cases.keys()
    if unknown:
        parser.error("unknown environment cases: " + ", ".join(sorted(unknown)))

    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="host-environment-", dir=args.output.resolve()))
    tool_env = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    failures = []
    records = []

    def run(label, command, environment):
        result = proc.run(command, cwd=work, env=environment, timeout=30)
        (work / f"{label}.stdout").write_bytes(result.stdout)
        (work / f"{label}.stderr").write_bytes(result.stderr)
        records.append({"name": label, "argv": result.argv, "exit": result.returncode,
                        "timed_out": result.timed_out})
        (work / "result.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        return result

    def observed(result):
        return result.returncode, result.stdout, result.stderr, result.timed_out

    version = run("version", [binary, "--version", "--json"], tool_env)
    if not version.ok or version.stderr:
        raise SystemExit(f"could not identify xray; artifacts: {work}")

    executables = {}
    for name, case in cases.items():
        if args.case and name not in args.case:
            continue
        source = fixtures / case["source"]
        if source not in executables:
            checked = run(source.stem + "-check", [binary, "check", source], tool_env)
            if observed(checked) != (0, b"", b"", False):
                failures.append((source.name, "Source check", observed(checked)))
            executable = work / (source.stem + (".exe" if os.name == "nt" else ""))
            command = [binary, "build", "--verbose", source, "-o", executable]
            if args.compiler:
                command += ["--cc", args.compiler]
            built = run(source.stem + "-build", command, tool_env)
            executables[source] = executable if built.ok and executable.is_file() else None
            if executables[source] is None:
                failures.append((source.name, "native build", observed(built)))

        runtime_env = tool_env.copy()
        for variable in case["unset_env"]:
            runtime_env.pop(variable, None)
        runtime_env.update(case["set_env"])
        output_key = "stdout_utf8"
        if output_key not in case:
            output_key = "stdout_windows_utf8" if os.name == "nt" else "stdout_posix_utf8"
        expected = (0, case[output_key].encode("utf-8"), b"", False)
        vm = observed(run(name + "-vm", [binary, "run", source], runtime_env))
        if vm != expected:
            failures.append((name, "VM result", vm, expected))
        if executable := executables[source]:
            native = observed(run(name + "-native", [executable], runtime_env))
            if native != expected or native != vm:
                failures.append((name, "native result and VM parity", native, vm, expected))

    if failures:
        for failure in failures:
            print(repr(failure), file=sys.stderr)
        raise SystemExit(f"{len(failures)} environment checks failed; artifacts: {work}")
    complete_native_artifacts(work)
    print(f"Hosted environment exits, stdout and stderr match on VM and native; artifacts: {work}")


if __name__ == "__main__":
    main()
