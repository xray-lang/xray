#!/usr/bin/env python3
"""Source-positioned refusals for methods reached through another module.

A method is selected through the nominal declaration that owns it. Using one
from a module that neither declares nor imports that owner used to fail inside
the constructed-module check, with no source position. These cases pin the two
source-level refusals and the ordinary imported call that must keep compiling.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

PARENT = """export class P {
    init() {}
    greet() -> string { return "p" }
}
"""

CHILD = """import { P } from "./parent"
export class C extends P {
    init() { super() }
}
export fn makeC() -> C { return C() }
"""

INHERITED = """import { makeC } from "./child"
const made = makeC()
const text = made.greet()
print(text)
"""

INHERITED_FIXED = """import { makeC } from "./child"
import { P } from "./parent"
const made = makeC()
const text = made.greet()
print(text)
"""

SECRET = """class S {
    init() {}
    value() -> i64 { return 7 }
}
export fn makeS() -> S { return S() }
"""

UNEXPORTED = """import { makeS } from "./secret"
const made = makeS()
print(made.value())
"""

IMPORTED = """import { P } from "./parent"
const made = P()
print(made.greet())
"""

INHERITED_VALUE = """import { makeC } from "./child"
const made = makeC()
const call = made.greet
print(call())
"""

BOTH = """class P {
    init() {}
    greet() -> string { return "p" }
}
export class C extends P {
    init() { super() }
}
export fn makeC() -> C { return C() }
"""

OWNER_UNEXPORTED = """import { makeC } from "./both"
const made = makeC()
print(made.greet())
"""

CASES = (
    ("inherited", {"parent.xr": PARENT, "child.xr": CHILD, "inherited.xr": INHERITED},
     "inherited.xr", "inherited.xr:3:19: error: method requires importing the module that declares it"),
    ("inherited_value", {"parent.xr": PARENT, "child.xr": CHILD, "inherited_value.xr": INHERITED_VALUE},
     "inherited_value.xr",
     "inherited_value.xr:3:19: error: method requires importing the module that declares it"),
    ("owner_unexported", {"both.xr": BOTH, "owner_unexported.xr": OWNER_UNEXPORTED},
     "owner_unexported.xr",
     "owner_unexported.xr:3:12: error: method owner type is not exported from its declaring module"),
    ("unexported", {"secret.xr": SECRET, "unexported.xr": UNEXPORTED},
     "unexported.xr", "unexported.xr:3:12: error: method receiver type is not exported from its declaring module"),
)

PASSING = (
    ("inherited_fixed", {"parent.xr": PARENT, "child.xr": CHILD, "inherited_fixed.xr": INHERITED_FIXED},
     "inherited_fixed.xr"),
    ("imported", {"parent.xr": PARENT, "imported.xr": IMPORTED}, "imported.xr"),
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="method-import-", dir=args.output.resolve()))
    environment = dict(os.environ, NO_COLOR="1", XRAY_BACKTRACE="0")
    if os.name == "nt":
        environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    failures = []

    def check(name, sources, entry):
        root = work / name
        root.mkdir()
        for filename, text in sources.items():
            (root / filename).write_text(text, encoding="utf-8", newline="\n")
        result = subprocess.run([str(binary), "check", str(root / entry)], cwd=root,
                                env=environment, capture_output=True, timeout=120)
        (root / "check.stdout").write_bytes(result.stdout)
        (root / "check.stderr").write_bytes(result.stderr)
        return result

    for name, sources, entry, expected in CASES:
        result = check(name, sources, entry)
        lines = result.stderr.decode("utf-8", "replace").splitlines()
        if result.returncode != 1 or not lines or not lines[0].endswith(expected):
            failures.append((name, expected, result.returncode, lines[:2]))
            continue
        root = work / name
        for label, command in (
            ("run", [str(binary), "run", str(root / entry)]),
            ("build -c", [str(binary), "build", "-c", str(root / entry), "-o", str(root / "refused.c")]),
        ):
            other = subprocess.run(command, cwd=root, env=environment, capture_output=True, timeout=120)
            other_lines = other.stderr.decode("utf-8", "replace").splitlines()
            if other.returncode != 1 or not other_lines or not other_lines[0].endswith(expected):
                failures.append((name + " preserves the refusal in " + label, expected,
                                 other.returncode, other_lines[:2]))
            if (root / "refused.c").exists():
                failures.append((name + " must not emit C after a Source refusal", expected, 0, []))

    for name, sources, entry in PASSING:
        result = check(name, sources, entry)
        lines = result.stderr.decode("utf-8", "replace").splitlines()
        if result.returncode != 0 or result.stderr:
            failures.append((name + " must stay accepted by the import check", 0, result.returncode, lines[:2]))

    if failures:
        for name, expected, code, lines in failures:
            print("FAIL %s: exit=%d expected=%r actual=%r" % (name, code, expected, lines), file=sys.stderr)
        raise SystemExit(1)
    print("method import visibility: %d refusals and %d accepted cases" % (len(CASES), len(PASSING)))


if __name__ == "__main__":
    main()
