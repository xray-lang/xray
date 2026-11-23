"""Exercise typed error calls through the register VM and a generated native executable."""

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
    parser.add_argument("--compiler", help="Use this native C provider")
    parser.add_argument("--fixtures", type=Path, default=Path("tests/fixtures/lir_cli/invoke_try_assert"),
                        help="Fixture directory relative to the source root")
    parser.add_argument("--case", action="append", default=[], help="Run only these named cases")
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    fixtures = root / args.fixtures
    cases = json.loads((fixtures / "cases.json").read_text(encoding="utf-8"))
    if not cases:
        parser.error("Error call fixtures and cases.json must match exactly")
    variant_sources = any("source" in case for case in cases.values())
    if variant_sources:
        for case in cases.values():
            filename = case.get("source", "")
            source = Path(filename)
            if source.name != filename or source.suffix != ".xr" or not (fixtures / source).is_file():
                parser.error("Each runtime variant must name an existing entry fixture")
    elif set(cases) != {path.stem for path in fixtures.glob("*.xr")}:
        parser.error("Error call fixtures and cases.json must match exactly")
    unknown = set(args.case) - cases.keys()
    if unknown:
        parser.error("Unknown error call cases: " + ", ".join(sorted(unknown)))
    by_source = {}
    for name, case in cases.items():
        if not args.case or name in args.case:
            source = fixtures / case.get("source", f"{name}.xr")
            by_source.setdefault(source, []).append((name, case))

    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="try-assert-", dir=args.output.resolve()))
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1", XRAY_BACKTRACE="0")
    if os.name == "nt":
        environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    for case in cases.values():
        for variable in case.get("set_env", {}):
            environment.pop(variable, None)
    failures = []

    def run(label, command, *, trace=False, set_env=None):
        child_env = environment.copy()
        if set_env is not None:
            child_env.update(set_env)
        if trace:
            child_env["XRAY_BACKTRACE"] = "1"
        result = proc.run(command, cwd=work, env=child_env, timeout=30)
        (work / f"{label}.stdout").write_bytes(result.stdout)
        (work / f"{label}.stderr").write_bytes(result.stderr)
        row = {"argv": result.argv, "exit": result.returncode, "timed_out": result.timed_out}
        if set_env is not None:
            row["set_env"] = set_env
        (work / f"{label}.json").write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
        if result.timed_out:
            failures.append((label, "timed out"))
        return result.returncode, result.stdout, result.stderr

    identity = run("version", [binary, "--version", "--json"])
    if identity[0] or identity[2]:
        raise SystemExit(f"could not identify xray; artifacts: {work}")

    stdout_key, stderr_key = ("stdout_utf8", "stderr_utf8") if variant_sources else ("stdout", "stderr")
    for source, variants in by_source.items():
        label = source.stem
        generated = work / f"{label}.c"
        checked = run(label + "-check", [binary, "check", source])
        vm_results = {
            name: run(name + "-vm", [binary, "run", source], set_env=case.get("set_env"))
            for name, case in variants
        }
        emitted = run(label + "-c", [binary, "build", "-c", source, "-o", generated])
        first_name, first_case = variants[0]
        if not variant_sources and "source_error" in first_case:
            expected = (source.name + first_case["source_error"]).encode("utf-8")
            lines = checked[2].splitlines()
            if checked[0] != 1 or checked[1] or not lines or not lines[0].endswith(expected):
                failures.append((first_name, "Source diagnostic", checked, expected))
            vm = vm_results[first_name]
            if vm != checked or emitted != checked or generated.exists():
                failures.append((first_name, "both backends must preserve the Source refusal", checked, vm, emitted))
            continue

        expected_results = {
            name: (case["exit"], case[stdout_key].encode("utf-8"), case[stderr_key].encode("utf-8"))
            for name, case in variants
        }
        if checked != (0, b"", b""):
            failures.append((label, "Source acceptance", checked))
        for name, case in variants:
            expected, vm = expected_results[name], vm_results[name]
            if vm != expected:
                failures.append((name, "VM result", vm, expected))
            if "trace" in case:
                traced = run(name + "-vm-trace", [binary, "run", source], trace=True, set_env=case.get("set_env"))
                frames = "".join(f"    at {source}:{line}:{column}\n" for line, column in case["trace"])
                trace_stderr = expected[2] + ("Stack trace:\n" + frames).encode("utf-8")
                trace_expected = (expected[0], expected[1], trace_stderr)
                if traced != trace_expected:
                    failures.append((name, "VM opt-in trace", traced, trace_expected))
        if emitted[0] or not generated.is_file():
            failures.append((label, "C emission", emitted))
            if not variant_sources:
                continue
        elif b"({" in generated.read_bytes():
            failures.append((label, "C output contains a GNU statement expression"))

        executable = work / (label + (".exe" if os.name == "nt" else ""))
        command = [binary, "build", source, "-o", executable]
        if args.compiler:
            command += ["--cc", args.compiler]
        built = run(label + "-build", command)
        if built[0] or not executable.is_file():
            failures.append((label, "native build", built))
            continue
        for name, case in variants:
            native = run(name + "-native", [executable], set_env=case.get("set_env"))
            vm, expected = vm_results[name], expected_results[name]
            if native != expected or native != vm:
                failures.append((name, "native result and VM parity", native, vm, expected))

    if failures:
        for failure in failures:
            print(repr(failure), file=sys.stderr)
        raise SystemExit(f"{len(failures)} error call checks failed; artifacts: {work}")
    complete_native_artifacts(work)
    print(f"Error call exits, output and any requested VM trace match expectations; artifacts: {work}")


if __name__ == "__main__":
    main()
