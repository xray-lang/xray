#!/usr/bin/env python3
"""Verify the exact scalar shared-slot representation boundary end to end."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
        check=False,
    )


def fail(message: str, result: subprocess.CompletedProcess[str] | None = None) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    if result is not None:
        if result.stdout:
            print(result.stdout, file=sys.stderr, end="")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    root = Path(args.root).resolve()
    source = root / "tests/aot/filetests/cgen/scalar_shared_representation_authority.xr"
    with tempfile.TemporaryDirectory(prefix="xray-scalar-shared-") as temporary:
        output = Path(temporary) / "scalar_shared.c"
        build = run(
            [args.xray, "build", "--native", "--c-only", "-o", str(output), str(source)],
            root,
        )
        if build.returncode != 0 or not output.is_file():
            return fail("production AOT driver did not emit generated C", build)

        generated = output.read_text(encoding="utf-8")
        required = (
            "XrValue xrt_shared[1]",
            "XR_FROM_INT(",
            "XR_TO_INT(xrt_shared[0])",
        )
        for needle in required:
            if needle not in generated:
                return fail(f"generated C lacks exact representation shape: {needle}")

        suffix = ".exe" if sys.platform == "win32" else ""
        program = Path(temporary) / f"scalar_shared{suffix}"
        compiled = run(
            [args.xray, "build", "--native", "-o", str(program), str(source)],
            root,
        )
        if compiled.returncode != 0 or not program.is_file():
            return fail("production native toolchain rejected generated C", compiled)
        executed = run([str(program)], root)
        if executed.returncode != 0 or executed.stdout != "42\n":
            return fail("native artifact did not preserve scalar shared-slot value", executed)

    print("scalar shared-slot generated-C authority gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
