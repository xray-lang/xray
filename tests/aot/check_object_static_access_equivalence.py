#!/usr/bin/env python3
"""Prove dot and static-bracket object access generate the same native C."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "dot": ROOT / "tests/benchmarks/aot/zero_cost/object_json/exact_dot_loop.xr",
    "bracket": ROOT
    / "tests/benchmarks/aot/zero_cost/object_json/exact_static_index_loop.xr",
}


def generate_c(xray: Path, source: Path, output: Path) -> str:
    proc = subprocess.run(
        [str(xray), "build", "--native", "-O", "2", "-c", "-o", str(output), str(source)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if proc.returncode != 0 or not output.is_file():
        raise RuntimeError(
            f"failed to generate C for {source.relative_to(ROOT)} (rc={proc.returncode})\n"
            f"{proc.stdout}"
        )
    return output.read_text(encoding="utf-8")


def normalize_generated_c(text: str) -> str:
    text = re.sub(
        r"exact_(?:dot|static_index)_loop_[0-9a-f]{16}", "object_access_case", text
    )
    return re.sub(
        r'[^"\r\n]*exact_(?:dot|static_index)_loop\.xr', "object_access_case.xr", text
    )


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {Path(argv[0]).name} <xray>", file=sys.stderr)
        return 2
    xray = Path(argv[1]).resolve()
    if not xray.is_file():
        print(f"missing xray executable: {xray}", file=sys.stderr)
        return 2

    try:
        with tempfile.TemporaryDirectory(prefix="xray-object-access-") as raw_tmp:
            tmp = Path(raw_tmp)
            generated = {
                name: generate_c(xray, source, tmp / f"{name}.c")
                for name, source in CASES.items()
            }
    except (OSError, RuntimeError) as exc:
        print(f"object static access equivalence: FAIL\n{exc}", file=sys.stderr)
        return 1

    for name, text in generated.items():
        object_body = re.sub(
            r"static XrValue xrt_runtime_record_new\([^}]+\}\n", "", text, flags=re.S
        )
        if "xrt_index_get" in text or "xrt_index_set" in text:
            print(
                f"object static access equivalence: FAIL\n{name} emitted generic index access",
                file=sys.stderr,
            )
            return 1
        if text.count("xrt_json_get_field") != 2:
            print(
                f"object static access equivalence: FAIL\n"
                f"{name} expected two fixed-field reads, got {text.count('xrt_json_get_field')}",
                file=sys.stderr,
            )
            return 1
        if text.count("static const XrtObjectShapeField _xobj_shape_fields_") != 1 or text.count(
            "static const XrtObjectShape _xobj_shape_"
        ) != 1:
            print(
                f"object static access equivalence: FAIL\n"
                f"{name} did not emit exactly one file-static shape descriptor",
                file=sys.stderr,
            )
            return 1
        if text.count("xrt_object_new_shape(&_xobj_shape_") != 1:
            print(
                f"object static access equivalence: FAIL\n"
                f"{name} did not construct the exact object from its static descriptor",
                file=sys.stderr,
            )
            return 1
        if (
            "xrt_struct_object_new_named" in object_body
            or "xrt_json_new_named" in object_body
            or "(const char*[]){" in object_body
            or "(const char *const[]){" in object_body
        ):
            print(
                f"object static access equivalence: FAIL\n"
                f"{name} retained a per-instance or block-scope shape table",
                file=sys.stderr,
            )
            return 1

    dot = normalize_generated_c(generated["dot"])
    bracket = normalize_generated_c(generated["bracket"])
    if dot != bracket:
        print(
            "object static access equivalence: FAIL\n"
            "normalized dot and static-bracket generated C differ",
            file=sys.stderr,
        )
        return 1

    print("object static access equivalence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
