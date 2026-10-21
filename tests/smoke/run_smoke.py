"""Run public examples with explicit, shrinking known-feature gaps."""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest import proc


@dataclass(frozen=True)
class Source:
    path: str
    group: str
    mode: str
    text: str | None = None


def readme_blocks(text: str) -> list[str]:
    """Extract every Xray fence, rejecting an unterminated example."""
    blocks, current, fence = [], None, None
    indentation = 0
    for line in text.splitlines(keepends=True):
        if fence is None:
            opener = re.fullmatch(r"( {0,3})(`{3,}|~{3,})([^\r\n]*)", line.rstrip("\r\n"))
            if opener:
                fence = opener[2]
                indentation = len(opener[1])
                language = opener[3].strip().split()
                current = [] if language and language[0] in {"xray", "xr"} else None
        elif re.fullmatch(r" {0,3}" + re.escape(fence[0]) + "{" + str(len(fence)) + r",}[ \t]*",
                          line.rstrip("\r\n")):
            if current is not None:
                blocks.append("".join(current))
            current, fence = None, None
        elif current is not None:
            removed = min(indentation, len(line) - len(line.lstrip(" ")))
            current.append(line[removed:])
    if current is not None:
        raise ValueError("unterminated Xray README fence")
    return blocks


def discover(root: Path) -> dict[str, Source]:
    sources = {}
    for path in sorted((root / "demos").rglob("*.xr")):
        relative = path.relative_to(root).as_posix()
        mode = "check" if "06-networking" in path.parts else "run"
        sources[relative] = Source(relative, "demos", mode)
    for name in ("README.md", "README_CN.md"):
        for number, block in enumerate(readme_blocks((root / name).read_text(encoding="utf-8")), 1):
            key = f"{name}#xray-{number}"
            sources[key] = Source(key, "readme", "run", block)
    for directory in sorted((root / "stdlib").iterdir()):
        if directory.name in {"_probe", "codegen"} or not directory.is_dir():
            continue
        entry = directory / f"{directory.name}.xr"
        if entry.is_file():
            key = entry.relative_to(root).as_posix()
            sources[key] = Source(key, "stdlib", "run", f"import {directory.name}\nprint(1)\n")
    return sources


def rooted_file(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f"missing or out-of-tree file: {relative}")
    return path


def load_cases(manifest: Path, root: Path, sources: dict[str, Source]) -> list[dict]:
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or not isinstance(data.get("cases"), list):
        raise ValueError("expected smoke manifest schema 1 and cases array")
    cases = data["cases"]
    ids, primary, native = set(), {}, {}
    for case in cases:
        for key in ("id", "path", "mode", "expect", "exit_code", "stdout"):
            if key not in case:
                raise ValueError(f"missing {key} in case {case!r}")
        name = case["id"]
        if not isinstance(name, str) or not name or name in ids:
            raise ValueError(f"empty or duplicate case id: {name}")
        ids.add(name)
        source = sources.get(case["path"])
        if source is None:
            raise ValueError(f"{name}: source is absent from discovery: {case['path']}")
        if case["mode"] not in {source.mode, "build-run"}:
            raise ValueError(f"{name}: mode does not match source (network demos are check-only)")
        if case["mode"] == "build-run" and source.mode != "run":
            raise ValueError(f"{name}: check-only source cannot be executed natively")
        target = native if case["mode"] == "build-run" else primary
        if source.path in target:
            raise ValueError(f"duplicate source/mode: {source.path}")
        target[source.path] = case
        rooted_file(root, case["stdout"])
        if case["expect"] == "pass":
            if case["exit_code"] != 0 or "gap" in case:
                raise ValueError(f"{name}: pass must exit zero without a gap allowance")
        elif case["expect"] == "gap":
            gap = case.get("gap", {})
            deferred = gap.get("deferred")
            if deferred:
                if (case["mode"] != "build-run" or gap.get("feature") != "Task328 P2"
                        or case["exit_code"] is not None or gap.get("first_diagnostic") is not None):
                    raise ValueError(f"{name}: only the pending native path can be deferred")
            elif case["exit_code"] != 1 or not gap.get("first_diagnostic"):
                raise ValueError(f"{name}: gap requires exit 1 and its first diagnostic")
            feature = gap.get("feature", "")
            if not re.fullmatch(r"F\d{2}|Task328 P2", feature):
                raise ValueError(f"{name}: gap requires a feature or Task328 P2")
            if case["mode"] == "build-run" and feature != "Task328 P2":
                raise ValueError(f"{name}: native gap must identify Task328 P2")
        else:
            raise ValueError(f"{name}: expect must be pass or gap")
    if set(primary) != set(sources):
        raise ValueError(f"unregistered sources: {sorted(set(sources) - set(primary))}")
    required_native = {path for path, case in primary.items()
                       if case["expect"] == "pass" and case["mode"] == "run"}
    if set(native) != required_native:
        raise ValueError(f"native companions differ: missing={sorted(required_native - set(native))}, "
                         f"extra={sorted(set(native) - required_native)}")
    for path, case in native.items():
        if case["stdout"] != primary[path]["stdout"]:
            raise ValueError(f"{case['id']}: native and VM must share the same stdout oracle")
    return sorted(cases, key=lambda case: case["id"])


def first_diagnostic(stderr: bytes, root: Path, source: Path, label: str) -> str:
    lines = stderr.decode("utf-8", errors="replace").splitlines()
    if not lines:
        return ""
    line = lines[0].replace("\\", "/")
    line = line.replace(source.as_posix(), label)
    return line.replace(root.as_posix() + "/", "")


def classify(case: dict, result: proc.ProcResult, diagnostic: str, expected: bytes,
             stage: str) -> tuple[str, str]:
    if result.timed_out:
        return "fail", f"{stage} timed out"
    if case["expect"] == "gap":
        if result.returncode == 0:
            return "fail", "gap unexpectedly passed; review output and promote the case"
        if result.returncode != case["exit_code"]:
            return "fail", f"{stage} exit {result.returncode}, expected {case['exit_code']}"
        if case["mode"] == "build-run" and stage != "build":
            return "fail", "native build gap changed to an execution failure"
        if diagnostic != case["gap"]["first_diagnostic"]:
            return "fail", f"first diagnostic changed: {diagnostic!r}"
        if result.stdout:
            return "fail", "known rejection unexpectedly produced stdout"
        return "gap", case["gap"]["feature"] + ": " + diagnostic
    if result.returncode != case["exit_code"]:
        return "fail", f"{stage} exit {result.returncode}: {diagnostic}"
    if case["mode"] == "build-run" and stage != "run":
        return "fail", "native executable was not produced"
    if result.stdout != expected:
        return "fail", f"stdout differs: expected {expected!r}, got {result.stdout!r}"
    if result.stderr:
        return "fail", f"unexpected stderr: {diagnostic}"
    return "pass", "output matches"


def execute(case: dict, spec: Source, binary: Path, root: Path, work: Path,
            logs: Path, timeout: float) -> dict:
    if case.get("gap", {}).get("deferred"):
        return {"id": case["id"], "group": "native", "status": "gap", "executed": False,
                "reason": "NOT_RUN: " + case["gap"]["deferred"], "commands": []}
    source = root / spec.path
    if spec.text is not None:
        source = work / "source.xr"
        source.write_text(spec.text, encoding="utf-8", newline="\n")
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    commands = []

    def invoke(stage: str, command: list) -> proc.ProcResult:
        result = proc.run(command, cwd=root, env=environment, timeout=timeout)
        (logs / f"{stage}.stdout").write_bytes(result.stdout)
        (logs / f"{stage}.stderr").write_bytes(result.stderr)
        commands.append({"stage": stage, "argv": [str(arg) for arg in command],
                         "exit_code": result.returncode, "timed_out": result.timed_out})
        return result

    started = time.monotonic()
    if case["mode"] == "build-run":
        executable = work / ("program.exe" if os.name == "nt" else "program")
        stage = "build"
        result = invoke(stage, [binary, "build", source, "-o", executable])
        if result.ok and executable.is_file():
            stage = "run"
            result = invoke(stage, [executable])
        elif result.ok:
            return {"id": case["id"], "group": "native", "status": "fail",
                    "reason": "successful build produced no executable", "commands": commands}
    else:
        stage = case["mode"]
        result = invoke(stage, [binary, stage, source])
    diagnostic = first_diagnostic(result.stderr, root, source, spec.path)
    expected = rooted_file(root, case["stdout"]).read_bytes()
    status, reason = classify(case, result, diagnostic, expected, stage)
    return {"id": case["id"], "group": "native" if case["mode"] == "build-run" else spec.group,
            "status": status, "reason": reason, "executed": True, "exit_code": result.returncode,
            "first_diagnostic": diagnostic, "commands": commands,
            "seconds": round(time.monotonic() - started, 3)}


def summary(results: list[dict]) -> str:
    fields = []
    for group in ("demos", "stdlib", "readme", "native"):
        selected = [row for row in results if row["group"] == group]
        fields.append(f"{group} {sum(row['status'] == 'pass' for row in selected)}/{len(selected)}")
    return "smoke: " + " ".join(fields)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    root, binary, output = args.root.resolve(), args.binary.resolve(), args.output.resolve()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    manifest = args.manifest or root / "tests/smoke/cases.json"
    output.mkdir(parents=True, exist_ok=True)
    report = output / "results.json"
    report.write_text(json.dumps({"schema": 1, "gate": "running", "binary": str(binary)}) + "\n",
                      encoding="utf-8")
    try:
        sources = discover(root)
        cases = load_cases(manifest, root, sources)
    except (OSError, ValueError, KeyError, TypeError) as error:
        report.write_text(json.dumps({"schema": 1, "gate": "fail", "error": str(error)}) + "\n",
                          encoding="utf-8")
        print(f"smoke: FAIL manifest: {error}", file=sys.stderr)
        return 1
    log_root = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    results = []
    for index, case in enumerate(cases):
        logs = log_root / f"{index:03d}"
        logs.mkdir()
        with tempfile.TemporaryDirectory(prefix="case-", dir=output) as directory:
            row = execute(case, sources[case["path"]], binary, root, Path(directory), logs, args.timeout)
        results.append(row)
        row["log_directory"] = str(logs)
        print(f"{row['status'].upper()} {row['id']}: {row['reason']}", flush=True)
    line = summary(results)
    counts = {status: sum(row["status"] == status for row in results) for status in ("pass", "gap", "fail")}
    counts["not_run"] = sum(row.get("executed") is False for row in results)
    report.write_text(json.dumps({"schema": 1, "gate": "fail" if counts["fail"] else "pass", "binary": str(binary),
        "summary": line, "counts": counts, "cases": results}, indent=2) + "\n", encoding="utf-8")
    print(line)
    print(f"smoke gate: pass={counts['pass']} gap={counts['gap']} fail={counts['fail']} "
          f"not_run={counts['not_run']}")
    return int(counts["fail"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
