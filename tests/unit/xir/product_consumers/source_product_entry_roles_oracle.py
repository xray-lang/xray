"""Run public source consumers against fixed independent expectations."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def require(condition, reason):
    if not condition:
        raise AssertionError(reason)


def exact(lines, prefix, expected):
    actual = [line for line in lines if line.startswith(prefix)]
    require(actual == expected, f"{prefix}: {actual!r} != {expected!r}")


def validate_output(case, returncode, stdout, stderr, generated_exists):
    name, oracle = case["case"], case["oracle"]
    lines = stdout.splitlines()
    require(returncode == oracle["returncode"], f"unexpected exit {returncode}")
    require(not stderr, f"unexpected stderr: {stderr}")
    compiler = [line for line in lines if line.startswith("compiler physical ")]
    require(len(compiler) == 1 and re.fullmatch(r"compiler physical blocks/bytes=0/0; peak=\d+", compiler[0]),
            "compiler physical release missing")
    exact(lines, "final runtime ", [f"final runtime physical blocks/bytes=0/0; attempts={oracle['runtime_attempts']}"])
    if "diagnostic" in oracle:
        diagnostic = oracle["diagnostic"]
        fields = " ".join(f"{key}={diagnostic[key]}" for key in
                          ["status", "stage", "source-status", "module", "line", "column",
                           "xir-status", "function", "block", "instruction", "reason"])
        exact(lines, "source-admission ", [f"source-admission case={name} {fields} message={diagnostic['message']}"])
        paths = [line.removeprefix("source-path ") for line in lines if line.startswith("source-path ")]
        require(len(paths) == 1 and Path(paths[0]).name == diagnostic["source_path_basename"], "wrong source path")
        require(not generated_exists, "rejected source emitted C")
        for prefix in ["projection ", "checked-function ", "atomic-control ", "atomic-execution ", "test-execution "]:
            exact(lines, prefix, [])
        return
    require(generated_exists, "successful projection did not emit C")
    facts = oracle["checked_facts"]
    exact(lines, "source-admission ", [f"source-admission case={name} status=0 "
          f"functions={facts['functions']} modules={facts['modules']} entry={facts['entry']}"])
    exact(lines, "projection ", [f"projection case={name} {oracle['projection']}"])
    controls = [f"atomic-control case={name} operation={item['operation']} ordering={item['ordering']} "
                f"tag={item['tag']} typed={item['typed']}" for item in oracle["controls"]]
    exact(lines, "atomic-control ", controls)
    execution = oracle.get("execution")
    if execution:
        exact(lines, "atomic-execution ", [f"atomic-execution case={name} instance={i} repeat={r} value=82"
              for i in range(2) for r in range(2)])
    else:
        exact(lines, "atomic-execution ", [])
    roles = oracle.get("test_roles")
    if roles:
        actual = [line for line in lines if line.startswith("source-test ")]
        require(len(actual) == 7, "root discovery count changed")
        functions = []
        for index, line in enumerate(actual):
            match = re.fullmatch(r"source-test index=(\d+) function=(\d+) role=(\d+) timeout=(\d+) name=(\w+)", line)
            require(match is not None, "malformed root role")
            at, function, role, timeout, role_name = match.groups()
            require((int(at), int(role), int(timeout), role_name) ==
                    (index, roles["roles"][index], roles["timeouts"][index], roles["names"][index]), "root role changed")
            functions.append(int(function))
        require(len(set(functions)) == 7, "role identities alias")
        exact(lines, "test-execution ", [f"test-execution instance={i} role-index={r} result=Unit"
              for i in range(2) for r in roles["sequence"]])
        exact(lines, "test-isolation ", [f"test-isolation instance={i} first-bump=41 second-read=41 "
              "dependency-denied=1 skip-denied=1" for i in range(2)])
    else:
        exact(lines, "source-test ", [])
        exact(lines, "test-execution ", [])
        exact(lines, "test-isolation ", [])
    exact(lines, "runtime physical ", [f"runtime physical blocks/bytes=0/0; attempts={oracle['runtime_attempts']}"]
          if name.startswith("legal_") else [])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    cases_path = Path(__file__).with_name("entry_roles") / "cases.json"
    case = next(item for item in json.loads(cases_path.read_text(encoding="utf-8"))["cases"] if item["case"] == args.case)
    root = args.fixtures.resolve() / args.case
    for name, binding in case["files"].items():
        raw = (root / name).read_bytes()
        require(len(raw) == binding["bytes"] and hashlib.sha256(raw).hexdigest() == binding["sha256"],
                f"fixture changed: {name}")
    scratch = args.scratch.resolve() / args.case
    scratch.mkdir(parents=True, exist_ok=True)
    generated = scratch / "generated.c"
    generated.unlink(missing_ok=True)
    command = [args.binary, args.case, str(root), str(root / "root.xr"), str(generated)]
    result = subprocess.run(command, capture_output=True, timeout=110)
    stdout = result.stdout.decode("utf-8", errors="strict")
    stderr = result.stderr.decode("utf-8", errors="strict")
    (scratch / "stdout.log").write_bytes(result.stdout)
    (scratch / "stderr.log").write_bytes(result.stderr)
    (scratch / "process.json").write_text(json.dumps({"command": command, "returncode": result.returncode}, indent=2) + "\n")
    validate_output(case, result.returncode, stdout, stderr, generated.exists() and generated.stat().st_size > 0)
    print(f"entry-role-consumer case={args.case} fixed-oracle=PASS native=NOT_RUN full-FI=NOT_RUN")


if __name__ == "__main__":
    main()
