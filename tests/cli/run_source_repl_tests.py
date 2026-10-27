#!/usr/bin/env python3
"""Exercise the original interactive commands through the real Source owner."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def require(condition: bool, detail: object) -> None:
    if not condition:
        raise AssertionError(detail)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--stdlib", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    environment = dict(os.environ, XRAY_STDLIB=str(args.stdlib.resolve()), NO_COLOR="1")
    with tempfile.TemporaryDirectory(prefix="xray-source-repl-") as temporary:
        root = Path(temporary)
        def invoke(source: bytes, *options: str) -> subprocess.CompletedProcess[bytes]:
            return subprocess.run([str(binary), "repl", *options], input=source, capture_output=True,
                                  cwd=root, env=environment, timeout=30, check=False)
        def run(source: str) -> subprocess.CompletedProcess[bytes]:
            result = invoke(source.encode("utf-8"), "--module-id", "cli-repl-tests")
            require(result.returncode == 0, (source, result.returncode, result.stdout, result.stderr))
            return result
        result = invoke(b".exit\n")
        require(result.returncode == 2 and b"--module-id" in result.stderr, result)
        result = run("var state=40\nfn next()->i64{state+=1;return state}\nnext()\n.vars\n.type next()\nstate\n.time next()\nstate\n.history\n.exit\n")
        require(not result.stderr and b"var   state : i64 = 41\n" in result.stdout and
                b"fn    next : function = <fn>\n" in result.stdout and
                b"xray> i64\nxray> 42\nxray> 43\n" in result.stdout and
                re.search(rb"[0-9]+\.[0-9]{3} ms\nxray> 43\n", result.stdout) and
                b"   1  var state=40\n" in result.stdout, result)
        result = run("var value=42\nfn old()->i64{return value}\nvar value=\"later\"\nold()\nvalue\n.reset\n.vars\n.quit\n")
        require(not result.stderr and b"xray> 42\nxray> later\n" in result.stdout and
                b"Environment reset\nxray>   (no bindings)\n" in result.stdout, result)
        result = run("fn sum()->i64 {\nreturn 42\n}\nsum()\n\"literal{\"\n/* open\ncomment */ 43\n.exit\n")
        require(not result.stderr and b"..... ..... xray> 42\n" in result.stdout and
                b"literal{\n" in result.stdout and b"..... 43\n" in result.stdout, result)
        (root / "sibling.xr").write_text("export fn answer()->i64{return 42}\n", encoding="utf-8")
        (root / "entry.xr").write_text('import "./sibling" as sibling\nvar loaded=sibling.answer()\nloaded\n', encoding="utf-8")
        result = run(".load entry.xr\nloaded\n.load missing.xr\nloaded\n.load\n.time\n.type\n.unknown\n.help\n.help syntax\n.help types\n.help coro\n.help operators\n.help unknown\n.clear\n.exit\n")
        require(b"Loading entry.xr...\n42\nxray> 42\n" in result.stdout and b"not-found" in result.stderr and
                b"Usage: .load" in result.stderr and b"Usage: .time" in result.stdout and
                b"Usage: .type" in result.stdout and b"Unknown command: .unknown" in result.stdout and
                b"Unknown topic: unknown" in result.stdout and b"\x1b[2J\x1b[H" in result.stdout, result)
        for command in (b".help", b".exit", b".quit", b".clear", b".reset", b".load", b".time", b".history", b".vars", b".type"):
            require(command in result.stdout, command)
        result = run('"' + "a" * 9000 + '"\n.exit\n')
        require(not result.stderr and b"a" * 9000 + b"\n" in result.stdout, result)
        result = invoke(b"42\0discard\n.exit\n", "--module-id", "cli-repl-tests")
        require(result.returncode == 1 and b"zero byte" in result.stderr, result)
        started = time.monotonic()
        result = run("import time\n{const before=time.monotonic();time.sleep(25);print(time.monotonic()-before>=25)}\nfn worker()->i64{Coro.yield();return 42}\ntry await go worker()\n.exit\n")
        require(not result.stderr and b"true\n" in result.stdout and b"42\n" in result.stdout and time.monotonic()-started >= 0.025, result)
    print("Source REPL CLI: ten commands, real FILE imports, old birth state, type effects, multiline/long input, strict bytes, timer and Task yield PASS")


if __name__ == "__main__":
    main()
