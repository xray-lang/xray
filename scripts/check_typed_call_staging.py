#!/usr/bin/env python3
"""Keep exact typed calls free of generic runtime argument staging."""

from __future__ import annotations

import argparse
from pathlib import Path
import re


FUNCTION = "copy_call_arguments"
FORBIDDEN = (
    r"\bXrDynValue\b",
    r"\bXrValue\b",
    r"\b(?:xr_malloc|xr_calloc|xr_realloc|malloc|calloc|realloc|alloca|_alloca)\s*\(",
)


def extract_function(source: str, name: str) -> str:
    signature = re.search(
        rf"static\s+XrTypedDispatchStatus\s+{re.escape(name)}\s*\(", source
    )
    if not signature:
        raise ValueError(f"missing {name} function")
    opening = source.find("{", signature.end())
    if opening < 0:
        raise ValueError(f"missing {name} body")
    depth = 0
    for offset in range(opening, len(source)):
        character = source[offset]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[signature.start():offset + 1]
    raise ValueError(f"unterminated {name} body")


def extract_bool_function(source: str, name: str) -> str:
    signature = re.search(
        rf"static\s+bool\s+{re.escape(name)}\s*\(", source
    )
    if not signature:
        raise ValueError(f"missing {name} function")
    opening = source.find("{", signature.end())
    if opening < 0:
        raise ValueError(f"missing {name} body")
    depth = 0
    for offset in range(opening, len(source)):
        character = source[offset]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[signature.start():offset + 1]
    raise ValueError(f"unterminated {name} body")


def verify(source: str) -> list[str]:
    errors: list[str] = []
    try:
        body = extract_function(source, FUNCTION)
        caller = extract_function(source, "execute_call")
        lifecycle = extract_bool_function(source, "function_has_zero_lifecycle")
    except ValueError as error:
        return [str(error)]
    required = (
        r"const\s+XrTargetCallArgumentRecord\s*\*arguments",
        r"uint16_t\s+argument_count",
        r"for\s*\(\s*uint16_t\s+ordinal\s*=\s*0\s*;\s*ordinal\s*<\s*argument_count\s*;\s*ordinal\+\+\s*\)",
        r"arguments\s*\[\s*ordinal\s*\]\s*\.\s*caller_slot",
        r"arguments\s*\[\s*ordinal\s*\]\s*\.\s*callee_slot",
    )
    for pattern in required:
        if not re.search(pattern, body):
            errors.append(f"{FUNCTION} lacks required direct-row pattern: {pattern}")
    if len(re.findall(r"\bfor\s*\(", body)) != 1:
        errors.append(f"{FUNCTION} must contain exactly one bounded loop")
    for pattern in FORBIDDEN:
        if re.search(pattern, body):
            errors.append(f"{FUNCTION} contains forbidden generic staging: {pattern}")
    if not re.search(
        r"copy_call_arguments\s*\(\s*frame\s*,\s*child\s*,\s*"
        r"call->argument_count\s*\?\s*&arguments\s*\[\s*call->argument_begin\s*\]"
        r"\s*:\s*NULL\s*,\s*call->argument_count\s*\)",
        caller,
    ):
        errors.append("execute_call does not pass the verified plan-row slice directly")
    lifecycle_required = (
        r"xr_target_plan_functions\s*\(",
        r"record->root_count\s*==\s*0",
        r"record->cleanup_count\s*==\s*0",
        r"record->coroutine_count\s*==\s*0",
    )
    for pattern in lifecycle_required:
        if not re.search(pattern, lifecycle):
            errors.append(
                "function_has_zero_lifecycle lacks O(1) partition proof: " + pattern
            )
    if re.search(r"\b(?:for|while)\s*\(", lifecycle):
        errors.append("function_has_zero_lifecycle must not scan lifecycle tables")
    return errors


def self_test() -> None:
    valid = """
static XrTypedDispatchStatus copy_call_arguments(
    XrTypedFrame *parent, XrTypedFrame *child,
    const XrTargetCallArgumentRecord *arguments, uint16_t argument_count) {
    for (uint16_t ordinal = 0; ordinal < argument_count; ordinal++) {
        load(parent, arguments[ordinal].caller_slot);
        store(child, arguments[ordinal].callee_slot);
    }
    return OK;
}
static XrTypedDispatchStatus execute_call(void) {
    copy_call_arguments(frame, child,
        call->argument_count ? &arguments[call->argument_begin] : NULL,
        call->argument_count);
}
static bool function_has_zero_lifecycle(const XrTargetPlan *plan,
                                        uint32_t function) {
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(plan, &count);
    const XrTargetFunctionRecord *record = &functions[function];
    return record->root_count == 0 && record->cleanup_count == 0 &&
           record->coroutine_count == 0;
}
"""
    if verify(valid):
        raise RuntimeError("valid direct-row fixture was rejected")
    invalid = valid.replace(
        "for (uint16_t ordinal", "XrDynValue *values = xr_calloc(1, 1);\nfor (uint16_t ordinal"
    )
    errors = verify(invalid)
    if len(errors) < 2:
        raise RuntimeError("generic staging fixture was not rejected")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("typed call staging self-test: PASS")
        return 0
    if not args.root:
        parser.error("--root is required")
    path = args.root / "src/vm/xr_typed_dispatch.c"
    errors = verify(path.read_text(encoding="utf-8"))
    if errors:
        for error in errors:
            print(f"typed call staging gate: {error}")
        return 1
    print("typed call staging gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
