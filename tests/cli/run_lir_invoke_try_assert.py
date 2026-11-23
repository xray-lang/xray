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
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    fixtures = root / args.fixtures
    cases = json.loads((fixtures / "cases.json").read_text(encoding="utf-8"))
    if not cases or set(cases) != {path.stem for path in fixtures.glob("*.xr")}:
        parser.error("Error call fixtures and cases.json must match exactly")

    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="try-assert-", dir=args.output.resolve()))
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1", XRAY_BACKTRACE="0")
    if os.name == "nt":
        environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    failures = []

    def run(label, command, *, trace=False):
        child_env = dict(environment, XRAY_BACKTRACE="1") if trace else environment
        result = proc.run(command, cwd=work, env=child_env, timeout=30)
        (work / f"{label}.stdout").write_bytes(result.stdout)
        (work / f"{label}.stderr").write_bytes(result.stderr)
        row = {"argv": result.argv, "exit": result.returncode, "timed_out": result.timed_out}
        (work / f"{label}.json").write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
        if result.timed_out:
            failures.append((label, "timed out"))
        return result.returncode, result.stdout, result.stderr

    identity = run("version", [binary, "--version", "--json"])
    if identity[0] or identity[2]:
        raise SystemExit(f"could not identify xray; artifacts: {work}")

    for name, case in cases.items():
        source = fixtures / f"{name}.xr"
        generated = work / f"{name}.c"
        checked = run(name + "-check", [binary, "check", source])
        vm = run(name + "-vm", [binary, "run", source])
        emitted = run(name + "-c", [binary, "build", "-c", source, "-o", generated])
        if "source_error" in case:
            expected = (source.name + case["source_error"]).encode("utf-8")
            lines = checked[2].splitlines()
            if checked[0] != 1 or checked[1] or not lines or not lines[0].endswith(expected):
                failures.append((name, "Source diagnostic", checked, expected))
            if vm != checked or emitted != checked or generated.exists():
                failures.append((name, "both backends must preserve the Source refusal", checked, vm, emitted))
            continue

        expected = (case["exit"], case["stdout"].encode("utf-8"), case["stderr"].encode("utf-8"))
        if checked != (0, b"", b""):
            failures.append((name, "Source acceptance", checked))
        if vm != expected:
            failures.append((name, "VM result", vm, expected))
        if "trace" in case:
            traced = run(name + "-vm-trace", [binary, "run", source], trace=True)
            frames = "".join(f"    at {source}:{line}:{column}\n" for line, column in case["trace"])
            trace_stderr = expected[2] + ("Stack trace:\n" + frames).encode("utf-8")
            trace_expected = (expected[0], expected[1], trace_stderr)
            if traced != trace_expected:
                failures.append((name, "VM opt-in trace", traced, trace_expected))
        if emitted[0] or not generated.is_file():
            failures.append((name, "C emission", emitted))
            continue
        if b"({" in generated.read_bytes():
            failures.append((name, "C output contains a GNU statement expression"))

        executable = work / (name + (".exe" if os.name == "nt" else ""))
        command = [binary, "build", source, "-o", executable]
        if args.compiler:
            command += ["--cc", args.compiler]
        built = run(name + "-build", command)
        if built[0] or not executable.is_file():
            failures.append((name, "native build", built))
            continue
        native = run(name + "-native", [executable])
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
