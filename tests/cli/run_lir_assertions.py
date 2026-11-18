"""Run unchanged assertion sources through both target backends with byte oracles."""
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
    parser.add_argument("--compiler", action="append", default=[])
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    fixtures = root / "tests/fixtures/lir_cli/assertions"
    cases = {path.name.removesuffix(".expect.json"): json.loads(path.read_text(encoding="utf-8"))
             for path in sorted(fixtures.glob("*.expect.json"))}
    if set(args.case) - set(cases):
        parser.error("unknown assertion case: " + ", ".join(sorted(set(args.case) - set(cases))))
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="assertions-", dir=args.output.resolve()))
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    if os.name == "nt":
        environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    records, failures = [], []

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
            failures.append((name, "exit", status, expected_exit))
        return status, paths["stdout"].read_bytes(), paths["stderr"].read_bytes()

    def oracle(name, source, expected, result):
        _, out, err = result
        if out != expected["stdout"].encode("utf-8"):
            failures.append((name, "stdout", out, expected["stdout"].encode("utf-8")))
        if "code" not in expected:
            if err:
                failures.append((name, "unexpected stderr", err))
            return
        prefix = (f'[Uncaught Panic] E{expected["code"]:04d}: ' + expected["message"] + "\n").encode("utf-8")
        if expected["code"] != 445:
            if err != prefix:
                failures.append((name, "exact panic", err, prefix))
            return
        markers = [(line, text) for line, text in enumerate(source.read_text(encoding="utf-8").splitlines(), 1)
                   if "// fault" in text]
        if len(markers) != 1:
            raise ValueError(f"{source}: one independent fault marker is required")
        line, text = markers[0]
        column = len(text) - len(text.lstrip()) + 1
        suffix = f"{source.name}:{line}:{column}\n".encode("utf-8")
        if not err.startswith(prefix + b"    at ") or not err.endswith(suffix):
            failures.append((name, "exact message and invocation source", err, prefix, suffix))

    for name, expected in cases.items():
        if args.case and name not in args.case:
            continue
        source = root / expected["source"] if "source" in expected else fixtures / (name + ".xr")
        if "reject" in expected:
            reference = run(name + "-check", [binary, "check", source], 1)
            if reference[1] or expected["reject"].encode("utf-8") not in reference[2]:
                failures.append((name, "Source type rejection", reference))
            for command_name, command in (
                    ("vm", [binary, "run", source]),
                    ("c", [binary, "build", "-c", source, "-o", work / (name + ".c")])):
                refused = run(name + "-" + command_name, command, 1)
                if refused != reference:
                    failures.append((name, "same first Source diagnostic", reference, refused))
            continue
        check = run(name + "-check", [binary, "check", source], 0)
        if check != (0, b"", b""):
            failures.append((name, "check", check))
            continue
        reference = run(name + "-vm", [binary, "run", source], expected["exit"])
        oracle(name + "-vm", source, expected, reference)
        generated = work / (name + ".c")
        emitted = run(name + "-c", [binary, "build", "-c", source, "-o", generated], 0)
        if emitted[0]:
            continue
        if "({" in generated.read_text(encoding="utf-8"):
            failures.append((name, "nonportable statement expression"))
        for ordinal, compiler in enumerate(args.compiler or [None]):
            executable = work / (name + f"-{ordinal}" + (".exe" if os.name == "nt" else ""))
            command = [binary, "build", source, "-o", executable]
            if compiler:
                command += ["--cc", compiler]
            built = run(name + f"-build-{ordinal}", command, 0)
            if built[0]:
                continue
            native = run(name + f"-native-{ordinal}", [executable], expected["exit"])
            oracle(name + f"-native-{ordinal}", source, expected, native)
            if native != reference:
                failures.append((name, "VM/native byte comparison", reference, native))
    if failures:
        for failure in failures:
            print(repr(failure), file=sys.stderr)
        raise SystemExit(f"assertion behavior failures: {len(failures)}; artifacts: {work}")
    complete_native_artifacts(work)
    print(f"Assertion behavior and source parity passed; artifacts: {work}")


if __name__ == "__main__":
    main()
