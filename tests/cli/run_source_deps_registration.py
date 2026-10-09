"""Exercise the real Source deps registry, help, options and parser rejection.

Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
Licensed under the MIT License.

This gate does not compile fixtures or invoke another compiler pipeline. Full
report bytes and Source admission are covered by test_cli_source_dependencies.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


ORIGINAL_COMMANDS = ("run", "test", "check", "fmt", "build", "info", "doctor", "self", "help")
WITHHELD_COMMANDS = ("pkg", "repl", "compile", "verify", "explain", "language")


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xray-deps-registry-") as temporary:
        root = Path(temporary)
        env = dict(os.environ, HOME=str(root), USERPROFILE=str(root), NO_COLOR="1")

        def invoke(args, code, diagnostic=None):
            result = subprocess.run([str(binary), *map(str, args)], cwd=root, env=env,
                                    capture_output=True, timeout=30)
            assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
            if diagnostic is not None:
                assert diagnostic in result.stderr, (args, result.stderr)
                assert not result.stdout, (args, result.stdout)
            return result

        usage = invoke(["--help"], 0).stdout.replace(b"\r\n", b"\n")
        commands = usage.split(b"Commands:\n", 1)[1].split(b"Global Options:", 1)[0]
        actual = re.findall(rb"^  ([a-z][a-z-]*)\s", commands.replace(b"\r\n", b"\n"), re.M)
        assert actual == [name.encode() for name in (*ORIGINAL_COMMANDS[:5], "deps", *ORIGINAL_COMMANDS[5:])], actual
        help_text = invoke(["deps", "--help"], 0).stdout
        assert help_text == invoke(["deps", "-h"], 0).stdout
        assert help_text == invoke(["help", "deps"], 0).stdout
        for short, long in (("o", "output"), ("s", "shell"), ("j", "json"), ("l", "list")):
            assert ("-" + short + ",").encode() in help_text
            assert ("--" + long).encode() in help_text
        for command in ORIGINAL_COMMANDS:
            invoke(["help", command], 0)
        for command in WITHHELD_COMMANDS:
            invoke([command], 2, ("unknown command '" + command + "'").encode())
        invoke(["deps"], 2, b"missing required argument")
        invoke(["deps", "one.xr", "two.xr"], 2, b"too many arguments")
        for args in (["--output"], ["-o"]):
            invoke(["deps", *args], 2, b"requires an argument")
        for option in ("--unknown-deps-option", "-Z"):
            invoke(["deps", "absent.xr", option], 2, b"unknown option")
        # Valid option spellings reach Source admission and fail there, not in parsing.
        # This absent entry is independent of stdlib availability and report formatting.
        for option in ("--shell", "-s", "--json", "-j", "--list", "-l"):
            for output in ("--output", "-o"):
                report = root / "occupied.report"
                report.write_bytes(b"registry-output\x00canary")
                invoke(["deps", root / "absent.xr", option, output, report], 1,
                       b"dependency analysis failed")
                assert report.read_bytes() == b"registry-output\x00canary"
        invoke(["deps", root / "absent.xr"], 1, b"dependency analysis failed")
    print("deps registry, original commands, help, options and fail-before-output PASS")


if __name__ == "__main__":
    main()
