#!/usr/bin/env python3
"""A freestanding provider object must stay inside its declared ABI.

The generated object for a freestanding coroutine target is compiled with the
provider ABI defines and then checked three ways: nothing outside the declared
allow-list may be left undefined, the case must still reach a core set of
provider hooks, and it must not carry the hosted value-ops bridge or reference
a hosted runtime symbol.

The first two are separate because a hook is referenced only when the lowered
program needs it, so equality against the whole allow-list would reject a legal
lowering. What equality did cover and the core set does not is noted on
REQUIRED_UNDEFINED.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary as binlib  # noqa: E402
from xraytest import platform, proc, toolchain, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
CASE = SCRIPT_DIR / "provider" / "freestanding_coro" / "main.xr"

SKIP_EXIT = 77

# Exactly the symbols a freestanding provider object may leave undefined.
ALLOWED_UNDEFINED = (
    "memcpy",
    "memset",
    "xr_aot_await_task",
    "xr_aot_await_task_resume",
    "xr_aot_frame_alloc",
    "xr_aot_frame_free",
    "xr_aot_run_main",
    "xr_aot_runtime_config_init",
    "xr_aot_runtime_delete",
    "xr_aot_runtime_new",
    "xr_aot_spawn",
    "xr_aot_trace_frame_value",
    "xr_runtime_core_enable_object_destroy_ops",
    "xr_runtime_core_enable_task_destroy_ops",
    "xrt_closure_new",
)

# The provider hooks this case must still reach. Membership in ALLOWED_UNDEFINED
# alone cannot carry that: several hooks are referenced only when the lowered
# program needs them -- a frame with no traced root never mentions the trace
# hook -- so requiring the whole list would fail on a legal lowering. Checking a
# core set instead keeps a case that stops reaching the provider from passing
# the escape test by referencing almost nothing.
#
# What the core set does not watch, and why, for each name in the difference:
#   memcpy, memset                     not provider hooks
#   xr_aot_runtime_config_init/new/    emitted together with xr_aot_run_main,
#     delete                           which the core set does require
#   xr_aot_await_task_resume           the other resume spellings are outside
#                                      ALLOWED, so a switch still escapes
#   xr_aot_trace_frame_value           UNWATCHED: this case traces no frame
#                                      root, so nothing pins frame-root tracing
#   xr_runtime_core_enable_object_/    UNWATCHED: driven by the OBJECTS and TASK
#     task_destroy_ops                 capability bits, and no gate ties the
#                                      declared capabilities to the emitted ones
REQUIRED_UNDEFINED = (
    "xr_aot_await_task",
    "xr_aot_frame_alloc",
    "xr_aot_frame_free",
    "xr_aot_run_main",
    "xr_aot_spawn",
    "xrt_closure_new",
)

if not set(REQUIRED_UNDEFINED) <= set(ALLOWED_UNDEFINED):
    raise AssertionError("required provider hooks must be declared as allowed")

HOSTED_SYMBOL_RE = re.compile(
    r"^(pthread|malloc$|free$|epoll|kqueue|netpoll|xray_rt_coro|xray_core)",
    re.IGNORECASE,
)

HOSTED_BRIDGE_RE = re.compile(r"xrt_runtime_value_ops|runtime_cfg\.value_ops")

PROVIDER_DEFINES = (
    "-DXRAY_TARGET_RUNTIME_PROVIDER=1",
    "-DXRAY_PROVIDER_ABI=1",
    "-DXRAY_PROVIDER_REQUIRED_CAPS=0x1021",
    "-DXRAY_PROVIDER_REQUIRED_HOOKS=0x17",
    "-DXRAY_PROVIDER_TARGET_METADATA_HASH=1",
)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Freestanding provider ABI gate")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    cc = toolchain.find_c_compiler()
    if cc is None:
        print("SKIP: no C compiler found")
        return SKIP_EXIT

    with workspace.Workspace("xray-provider-abi") as ws:
        provider_c = ws.path("provider.c")
        result = proc.run(
            [xray, "build", "--native", "--target", "native", "-c", "-o", provider_c, CASE],
            timeout=timeout,
        )
        if not result.ok:
            sys.stderr.write("provider C generation failed\n")
            sys.stderr.write(result.combined_text()[:8000])
            return 1

        generated = provider_c.read_text(encoding="utf-8")
        if HOSTED_BRIDGE_RE.search(generated):
            sys.stderr.write("freestanding provider object emitted the hosted value-ops bridge\n")
            return 1

        provider_o = ws.path("provider.o")
        compile_result = proc.run(
            [cc.path, "-std=c11", "-ffreestanding", "-fno-stack-protector",
             *PROVIDER_DEFINES,
             f"-I{PROJECT_DIR / 'src' / 'aot'}", f"-I{PROJECT_DIR / 'include'}",
             "-c", provider_c, "-o", provider_o],
            timeout=timeout,
        )
        if not compile_result.ok:
            sys.stderr.write("provider object failed to compile\n")
            sys.stderr.write(compile_result.combined_text()[:8000])
            return 1

        undefined = binlib.undefined_symbol_names(provider_o, timeout=timeout)
        if undefined is None:
            print("SKIP: nm not available")
            return SKIP_EXIT

        escaped = sorted(set(undefined) - set(ALLOWED_UNDEFINED))
        if escaped:
            sys.stderr.write("freestanding provider object escaped its declared ABI\n")
            for name in escaped:
                sys.stderr.write(f"  + {name}\n")
            return 1

        unreached = sorted(set(REQUIRED_UNDEFINED) - set(undefined))
        if unreached:
            sys.stderr.write("freestanding provider object no longer reaches its provider ABI\n")
            for name in unreached:
                sys.stderr.write(f"  - {name}\n")
            return 1

        hosted = [n for n in undefined if HOSTED_SYMBOL_RE.search(n)]
        if hosted:
            sys.stderr.write("freestanding provider object references a hosted runtime symbol\n")
            for name in hosted:
                sys.stderr.write(f"  {name}\n")
            return 1

    print("freestanding provider ABI test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
