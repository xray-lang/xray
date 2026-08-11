#!/usr/bin/env python3
"""Verify immutable String-literal representation authority end to end."""

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
    source = root / "tests/aot/filetests/cgen/string_literal_representation_authority.xr"
    with tempfile.TemporaryDirectory(prefix="xray-string-literal-authority-") as temporary:
        output = Path(temporary) / "string_literal.c"
        built = run(
            [args.xray, "build", "--native", "--c-only", "-o", str(output), str(source)],
            root,
        )
        if built.returncode != 0 or not output.is_file():
            return fail("production AOT driver did not emit generated C", built)

        generated = output.read_text(encoding="utf-8")
        required = (
            "static const xrt_str_t",
            "INT64_C(5), INT64_C(5)",
            "XRT_STR_LITERAL",
            '(char *) "Hello"',
            "XrValue v0 = xr_str_lit(",
            "xrt_println(xr_str_lit(",
        )
        for needle in required:
            if needle not in generated:
                return fail(f"generated C lacks exact String-literal authority: {needle}")
        if "xrt_str_from_cstr" in generated:
            return fail("generated C reconstructed the literal through a legacy String fallback")

        suffix = ".exe" if sys.platform == "win32" else ""
        program = Path(temporary) / f"string_literal{suffix}"
        compiled = run(
            [args.xray, "build", "--native", "--rebuild", "-o", str(program), str(source)],
            root,
        )
        if compiled.returncode != 0 or not program.is_file():
            return fail("production native toolchain rejected generated C", compiled)
        executed = run([str(program)], root)
        if executed.returncode != 0 or executed.stdout != "Hello\n":
            return fail("native artifact did not preserve exact literal bytes", executed)

    print("String-literal generated-C authority gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
