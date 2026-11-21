"""Run the Range members the specification declares, on both backends.

Section 14.12 lists `contains`, `toArray`, `toString` and `iterator` beside
`count`; `iterator()` is not covered here because its return type has to match the
iterator protocol, which is a separate decision. The program below is written once
and executed twice, once through the VM and once as a native binary, and both
outputs must equal the same independently written answer.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

PROGRAM = """var pages = 1..=3
print(pages.contains(3))
print(pages.contains(4))
print(pages.contains(0))
print(len(pages))
print(pages.toString())
var listed = pages.toArray()
print(len(listed))
print(listed[0])
print(listed[2])
var half = 1..3
print(half.contains(3))
print(half.toString())
var empty = 5..5
print(len(empty))
print(empty.toString())
"""

EXPECTED_STDOUT = (
    "true\n"
    "false\n"
    "false\n"
    "3\n"
    "1..=3\n"
    "3\n"
    "1\n"
    "3\n"
    "false\n"
    "1..3\n"
    "0\n"
    "5..5\n"
)


def run(command, cwd, timeout=120):
    started = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
    return started


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="xray binary under test")
    parser.add_argument("--root", default=None,
                        help="source root; defaults to the checkout containing this script")
    arguments = parser.parse_args()
    if not arguments.root:
        arguments.root = str(Path(__file__).resolve().parents[2])

    environment = dict(os.environ, XRAY_STDLIB_PATH=str(Path(arguments.root) / "stdlib"),
                       NO_COLOR="1")
    with tempfile.TemporaryDirectory(prefix="xray-range-") as workspace:
        source = Path(workspace) / "range_members.xr"
        source.write_text(PROGRAM, encoding="utf-8", newline="\n")
        failures = []

        vm = run([arguments.binary, "run", str(source)], arguments.root)
        if vm.returncode != 0:
            failures.append(f"VM exit {vm.returncode}: {vm.stderr.decode('utf-8', 'replace').strip()}")
        elif vm.stdout.decode("utf-8", "replace") != EXPECTED_STDOUT:
            failures.append(f"VM stdout differs: {vm.stdout!r}")
        elif vm.stderr:
            failures.append(f"VM wrote stderr: {vm.stderr!r}")

        executable = Path(workspace) / "range_members.exe"
        built = run([arguments.binary, "build", str(source), "-o", str(executable)], arguments.root)
        if built.returncode != 0 or not executable.is_file():
            failures.append(f"native build exit {built.returncode}: "
                            f"{built.stderr.decode('utf-8', 'replace').strip()}")
        else:
            native = run([str(executable)], arguments.root)
            if native.returncode != 0:
                failures.append(f"native exit {native.returncode}")
            elif native.stdout.decode("utf-8", "replace") != EXPECTED_STDOUT:
                failures.append(f"native stdout differs: {native.stdout!r}")
            elif native.stderr:
                failures.append(f"native wrote stderr: {native.stderr!r}")

        if failures:
            for failure in failures:
                print(f"FAIL {failure}", file=sys.stderr)
            return 1
    print("range members: VM and native agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
