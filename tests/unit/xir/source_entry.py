"""Check entry and imported-module statements with the real Source producer."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--library-producer", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TEMP=str(args.output), TMP=str(args.output),
               XRAY=str(args.binary), XRAY_TEST_JOBS="1", PYTHONDONTWRITEBYTECODE="1")
    rows = []

    def run(name: str, command: list[str], expected: bytes | None = None,
            rejected: bool = False) -> subprocess.CompletedProcess[bytes]:
        value = subprocess.run(command, cwd=args.root, env=env, capture_output=True, timeout=30)
        (args.output / (name + ".stdout")).write_bytes(value.stdout)
        (args.output / (name + ".stderr")).write_bytes(value.stderr)
        passed = value.returncode != 0 and b"E0335" in value.stderr if rejected else value.returncode == 0 and not value.stderr
        if expected is not None:
            passed = passed and value.stdout == expected
        rows.append(dict(name=name, exit=value.returncode, passed=passed))
        if not passed:
            print(name, value.returncode, value.stdout, value.stderr)
        return value

    peer = args.output / "peer.xr"
    peer.write_text('fn start()->i64{print("init");return 40}\nconst state=start()\nexport fn value()->i64{return state+2}\n', encoding="utf-8")
    cases = {
        "state_and_entry": ('import "./peer" as peer\nfn main(){print("unexpected")}\nif(peer.value()==42){print("entry")}\n', b"init\nentry\n"),
        "ordinary_main": ('fn main()->i64{return 7}\nprint(main())\n', b"7\n"),
    }
    for name, (text, expected) in cases.items():
        source = args.output / (name + ".xr")
        source.write_text(text, encoding="utf-8")
        run(name + ".check", [str(args.binary), "check", str(source)])
        run(name + ".run", [str(args.binary), "run", str(source)], expected)
    fixture = args.root / "tests/compile_errors/error_entry/fixtures/standalone_call.xr"
    run("same_file_as_entry", [str(args.binary), "run", str(fixture)], b"call\n")

    command = [sys.executable, "-X", "utf8", str(args.root / "tests/compile_errors/run_compile_error_tests.py")]
    for case in ("import_call", "import_control", "transitive_control"):
        command.extend(("--case", "error_entry/" + case + ".xr"))
    negative = run("imported_diagnostics", command)
    if negative.returncode:
        print(negative.stdout.decode("utf-8", errors="replace"))

    for name, text, rejected in (
        ("library_declarations", 'fn start()->i64{return 7}\nconst state=start()\nexport fn value()->i64{return state}\n', False),
        ("library_mutable_initialization", 'fn start()->i64{return 7}\nvar state=start()\nexport fn value()->i64{return state}\n', False),
        ("library_statement", 'export fn value()->i64{return 7}\nprint("bad")\n', True),
    ):
        source = args.output / (name + ".xr")
        packet = args.output / (name + ".chk")
        source.write_text(text, encoding="utf-8")
        run(name, [str(args.library_producer), str(args.output), str(source), "source-entry-tests", str(packet)], rejected=rejected)

    (args.output / "result.json").write_text(json.dumps(dict(rows=rows), indent=2) + "\n", encoding="utf-8")
    return 0 if all(row["passed"] for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
