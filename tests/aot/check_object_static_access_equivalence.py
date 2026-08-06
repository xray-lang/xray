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
CODEC_CASE = ROOT / "tests/benchmarks/aot/zero_cost/object_json/json_encode_stringify.xr"


def generate_c(xray: Path, source: Path, output: Path) -> str:
    proc = subprocess.run(
        [str(xray), "build", "--native", "-O", "2", "-c", "-o", str(output), str(source)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="strict",
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
            codec = generate_c(xray, CODEC_CASE, tmp / "codec.c")
    except (OSError, RuntimeError) as exc:
        print(f"object static access equivalence: FAIL\n{exc}", file=sys.stderr)
        return 1

    for name, text in generated.items():
        run_match = re.search(
            r"static (?:XR_AINLINE )?int64_t [^(]+_run_1\([^)]*\) \{.*?^\}",
            text,
            flags=re.S | re.M,
        )
        if not run_match:
            print(
                f"object static access equivalence: FAIL\n{name} has no generated run body",
                file=sys.stderr,
            )
            return 1
        object_body = run_match.group(0)
        if "xrt_index_get" in object_body or "xrt_index_set" in object_body:
            print(
                f"object static access equivalence: FAIL\n{name} emitted generic index access",
                file=sys.stderr,
            )
            return 1
        if object_body.count("xrt_json_get_field") != 4:
            print(
                f"object static access equivalence: FAIL\n"
                f"{name} expected four fixed-field reads, got "
                f"{object_body.count('xrt_json_get_field')}",
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
        if object_body.count("xrt_object_new_static_shape(&_xobj_shape_") != 1:
            print(
                f"object static access equivalence: FAIL\n"
                f"{name} did not construct the exact object from its static descriptor",
                file=sys.stderr,
            )
            return 1
        if not object_body.startswith("static XR_AINLINE int64_t "):
            print(
                f"object static access equivalence: FAIL\n"
                f"{name} lost the small exact-object loop native-inline contract",
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

    if codec.count("static const XrtObjectShape _xobj_shape_") != 2:
        print(
            "object static access equivalence: FAIL\n"
            "static object encoding did not emit structural and Json-domain descriptors",
            file=sys.stderr,
        )
        return 1
    if "xrt_json_encode_static_object(" not in codec:
        print(
            "object static access equivalence: FAIL\n"
            "static object encoding rebuilt its Json-domain shape at run time",
            file=sys.stderr,
        )
        return 1
    codec_run = re.search(
        r"static (?:XR_AINLINE )?int64_t [^(]+_run_1\([^)]*\) \{.*?^\}",
        codec,
        flags=re.S | re.M,
    )
    if not codec_run or codec_run.group(0).startswith("static XR_AINLINE int64_t "):
        print(
            "object static access equivalence: FAIL\n"
            "Json codec loop crossed the narrow static-field force-inline boundary",
            file=sys.stderr,
        )
        return 1

    print("object static access equivalence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
