"""Require the public build help, parser and actual C publication to agree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest import proc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    source = work / "main.xr"
    source.write_bytes(b"print(42)\n")
    config_root = work / "private-config"
    env = dict(os.environ, LOCALAPPDATA=str(config_root), NO_COLOR="1",
               XRAY_STDLIB_PATH=str(args.root.resolve() / "stdlib"))
    commands = []
    state = dict(state="RUNNING", commands=commands)
    def fact(path):
        data = path.read_bytes()
        return dict(path=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    def save():
        (work / "result.json").write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    def invoke(name, arguments, expected):
        result = proc.run([args.binary, *arguments], cwd=work, env=env, timeout=30)
        for stream in ("stdout", "stderr"):
            (work / (name + "." + stream)).write_bytes(getattr(result, stream))
        commands.append(dict(name=name, args=list(map(str, arguments)), exit=result.returncode,
                             timedOut=result.timed_out, stdout=fact(work / (name + ".stdout")),
                             stderr=fact(work / (name + ".stderr"))))
        save()
        assert result.returncode == expected and not result.timed_out, commands[-1]
        return result
    save()
    helps = [invoke("help-command", ["help", "build"], 0), invoke("help-option", ["build", "--help"], 0)]
    assert helps[0].stdout == helps[1].stdout and all(not x.stderr for x in helps)
    assert set(re.findall(rb"--([a-z][a-z-]*)", helps[0].stdout)) == {
        b"output", b"c-only", b"cc", b"native", b"verbose", b"help"}
    toolchain = invoke("toolchain-help", ["help", "toolchain"], 0)
    assert b"--provider" in toolchain.stdout and b"--profile" in toolchain.stdout and b"--cc" in toolchain.stdout
    retired = ["c-dialect=c11", "opt=2", "debug", "cpu=native", "simd=scalar", "sysroot=unused",
               "strip", "profile=hosted", "type-names=none", "artifact=executable", "target=native",
               "toolchain=auto", "toolchain=msvc", "toolchain=clang", "toolchain=gcc", "toolchain=zig",
               "zig=unused", "dump-xaot-plan", "dump-global-evidence", "dump-xi-evidence", "dump-link-manifest",
               "dump-residue", "dump-link-command", "dump-toolchain-plan", "dry-run-link", "linker-script=unused",
               "c-header=unused", "c-export-prefix=unused", "c-export-exclude=unused", "keep-c", "cache-dir=unused",
               "rebuild", "lto", "rc-guard"]
    protected = work / "preserved-output"
    sentinel = b"unmodified output\x00\xff"
    protected.write_bytes(sentinel)
    for i, flag in enumerate(retired):
        rejected = invoke("retired-" + str(i), ["build", "--" + flag, source, "-o", protected], 2)
        assert b"unknown option" in rejected.stderr and not rejected.stdout
        assert protected.read_bytes() == sentinel
    for label, flags in [("long", ["--c-only", "--verbose"]),
                         ("short", ["-N", "-c", "-v"]),
                         ("global", ["--c-only"])]:
        output = work / (label + ".c")
        arguments = ["build", *flags, source, "-o", output]
        if label == "global":
            arguments.insert(0, "--verbose")
        emitted = invoke("emit-" + label, arguments, 0)
        assert emitted.stdout == ("Generated: " + str(output) + "\n").encode()
        assert b"XR_BUILD: Source " in emitted.stderr and b"XR_BUILD: C projection " in emitted.stderr
        assert b"xray_app" in output.read_bytes()
    assert (work / "long.c").read_bytes() == (work / "short.c").read_bytes() == (work / "global.c").read_bytes()
    invalid = invoke("compiler-with-c-only", ["build", "-c", "--cc", "unused-cl.exe", source, "-o", protected], 1)
    assert b"unsupported Source build option --cc" in invalid.stderr and protected.read_bytes() == sentinel
    missing = invoke("missing-explicit-compiler", ["build", "--native", "--cc", work / "missing-cl.exe", source, "-o", protected], 1)
    assert b"XR_BUILD_6006" in missing.stderr and protected.read_bytes() == sentinel
    for name in ("output", "cc"):
        invoke("missing-" + name, ["build", source, "--" + name], 2)
    assert source.read_bytes() == b"print(42)\n" and not (config_root / "Xray/toolchains.toml").exists()
    state.update(state="PASS", source=fact(source), cSource=fact(work / "long.c"))
    save()
    print("Build option help/parser/publication, retired flags and compiler rules PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
