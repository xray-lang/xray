#!/usr/bin/env python3
"""Reject legacy materialized-string ABI dependencies after the hard cut."""

from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def reject(relative: str, tokens: tuple[str, ...], errors: list[str]) -> None:
    text = (ROOT / relative).read_text(encoding="utf-8")
    for token in tokens:
        if token in text:
            errors.append(f"{relative}: forbidden token {token!r}")


def require(relative: str, tokens: tuple[str, ...], errors: list[str]) -> None:
    text = (ROOT / relative).read_text(encoding="utf-8")
    for token in tokens:
        if token not in text:
            errors.append(f"{relative}: missing canonical token {token!r}")


def main() -> int:
    errors: list[str] = []
    reject(
        "src/runtime/object/xstring.h",
        ("XrObjHeader", "XrObjType", "XR_OBJ_", " XrString.hdr"),
        errors,
    )
    for relative in (
        "src/runtime/object/xstring.c",
        "src/runtime/object/xstring_pool.c",
    ):
        reject(relative, ("->hdr", "xr_coro_heap_new_obj", "xr_sysheap_alloc_shared"), errors)
        require(relative, ("xr_runtime_object_", "XR_RUNTIME_STRING_"), errors)

    reject(
        "src/stdlib/xstdlib_vm_fastpath.c",
        ("return XR_FROM_PTR(string);",),
        errors,
    )
    require(
        "src/stdlib/xstdlib_vm_fastpath.c",
        ("return xr_string_value(string);",),
        errors,
    )
    reject(
        "src/runtime/object/xpanic_info.c",
        (
            "XR_FROM_PTR(msg)",
            "XR_FROM_PTR(error->message)",
            "XR_FROM_PTR(msg_str)",
            "return result ? XR_FROM_PTR(result)",
        ),
        errors,
    )
    require(
        "src/runtime/object/xpanic_info.c",
        ("return result ? xr_string_value(result)",),
        errors,
    )
    require(
        "src/vm/xvm_dispatch_helpers.h",
        (
            "if (XR_IS_STRING(receiver))",
            "native_type_classes[XR_TSTRING]",
        ),
        errors,
    )

    reject(
        "src/aot/xaot_coro.h",
        ("XrAotRuntimeStringView", "xr_str_hdr", "const xrt_str_t *src"),
        errors,
    )
    require(
        "src/aot/xaot_coro.h",
        ("xr_runtime_string_object_validate_prefix",),
        errors,
    )
    for relative in ("src/coro/xdeep_copy.c", "src/coro/xaot_runtime.c"):
        reject(relative, ("XrAotStringView",), errors)
        require(
            relative,
            (
                "XrAotLiteralStringView",
                "XR_AOT_VALUE_TAG_STR_ARC",
                "xr_runtime_string_object_validate_prefix",
            ),
            errors,
        )
    require(
        "src/runtime/mem/xweak_handle.c",
        ("XR_WEAK_TARGET_RUNTIME_STRING", "xr_runtime_object_register_weak"),
        errors,
    )
    require(
        "src/runtime/mem/xruntime_object_heap.c",
        ("xr_weak_table_runtime_target_dying",),
        errors,
    )
    arc = (ROOT / "src/aot/xrt_arc.h").read_text(encoding="utf-8")
    start = arc.find("static inline XrValue xrt_str_alloc")
    end = arc.find("static inline XrValue xrt_str_concat", start)
    if start < 0 or end < 0:
        errors.append("src/aot/xrt_arc.h: canonical string constructor boundary missing")
    else:
        constructor = arc[start:end]
        for token in ("xrt_arc_alloc", "xrt_str_t", "XrObjHeader"):
            if token in constructor:
                errors.append(
                    f"src/aot/xrt_arc.h string constructor: forbidden token {token!r}"
                )
        for token in (
            "XrString",
            "XRT_EXECUTION_OBJECT_RUNTIME_STRING",
            "xr_runtime_string_object_init",
            "XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL",
            "XR_RUNTIME_STRING_TRAIT_LOCAL",
        ):
            if token not in constructor:
                errors.append(
                    f"src/aot/xrt_arc.h string constructor: missing canonical token {token!r}"
                )

    anchor = (ROOT / "contracts/target-machine/runtime-string-object-contract.toml").read_text(
        encoding="utf-8"
    )
    kat = "a87ef4c611b54f7bcee9396d8226ee1dc31c80e9b907ffb0c4d8b51811ba6596"
    if f'fingerprint = "{kat}"' not in anchor:
        errors.append("runtime string contract anchor fingerprint drift")

    if errors:
        print("runtime string object boundary failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("runtime string object boundary passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
