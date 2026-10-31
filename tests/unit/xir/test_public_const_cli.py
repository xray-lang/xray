"""Exercise complete public constants through the existing VM and native CLI."""
from __future__ import annotations
import argparse
import hashlib
import json
import sys
import tempfile
from pathlib import Path
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest import proc

def fact(path: Path) -> dict:
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    report = work / "result.json"
    files = [args.fixtures / name for name in ("main-supported.xr", "values-supported.xr", "expected.out", "main.xr", "values.xr", "callables-main.xr", "callables.xr", "callables.out")]
    before = [fact(path) for path in files]
    expected = files[2].read_bytes()
    commands = []
    state = {"state": "RUNNING", "inputs": before, "commands": commands}
    def save() -> None:
        report.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    def invoke(label: str, argv: list, stdout_expected: bytes | None) -> bool:
        result = proc.run(argv, cwd=args.fixtures, timeout=30)
        (work / (label + ".stdout")).write_bytes(result.stdout)
        (work / (label + ".stderr")).write_bytes(result.stderr)
        row = {"name": label, "argv": [str(x) for x in argv], "exit": result.returncode,
               "timedOut": result.timed_out, "stdout": fact(work / (label + ".stdout")),
               "stderr": fact(work / (label + ".stderr"))}
        row["pass"] = result.ok and (stdout_expected is None or result.stdout == stdout_expected)
        commands.append(row)
        save()
        return row["pass"]
    save()
    state["historicalObjectInput"] = "Original complete object-literal Source is retained as a baseline, outside this gate."
    vm = invoke("vm", [args.binary, "run", files[0]], expected)
    executable = work / "public-const.exe"
    built = invoke("native-build", [args.binary, "build", files[0], "-o", executable], None)
    native = invoke("native", [executable], expected) if built and executable.is_file() else False
    rejected_stderr = (b"error: constructed XIR failed checking at function 4294967295 block 4294967295 instruction 4294967295\n"
                       b"source build failed (stage=4 status=source-rejected)\n")
    callable_rejections = []
    for mode in ("run", "build"):
        argv = [args.binary, mode, files[5]]
        if mode == "build":
            argv += ["-o", work / "non-sendable.exe"]
        result = proc.run(argv, cwd=args.fixtures, timeout=30)
        (work / ("callable-" + mode + ".stdout")).write_bytes(result.stdout)
        (work / ("callable-" + mode + ".stderr")).write_bytes(result.stderr)
        accepted = (result.returncode == 1 and not result.timed_out and
                    not result.stdout and result.stderr == rejected_stderr)
        callable_rejections.append(accepted)
        commands.append({"name": "non-sendable-callable-" + mode, "argv": [str(a) for a in argv],
                         "exit": result.returncode, "timedOut": result.timed_out, "pass": accepted,
                         "stdout": fact(work / ("callable-" + mode + ".stdout")),
                         "stderr": fact(work / ("callable-" + mode + ".stderr"))})
        save()
    state["protected"] = before == [fact(path) for path in files]
    state["state"] = "PASS" if vm and built and native and all(callable_rejections) and state["protected"] else "FAIL"
    save()
    print(json.dumps({"state": state["state"], "result": str(report)}))
    return 0 if state["state"] == "PASS" else 1

if __name__ == "__main__":
    raise SystemExit(main())
