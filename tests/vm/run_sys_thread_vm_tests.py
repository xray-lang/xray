#!/usr/bin/env python3
"""VM `sys.Thread`: results, and the handle-lifecycle diagnostics.

Two case tables, and the split is the point:

  OUTPUT_CASES  must run clean -- exact stdout and EMPTY stderr. A lifecycle
                warning leaking into one of these would mean the analyzer
                cannot see that the handle is joined, so silence is part of
                the assertion, not incidental.
  WARNING_CASES must run clean AND emit every named diagnostic. These are the
                shapes where a handle escapes without join/detach; the program
                still succeeds, so the warning text is the only observable.

Together they pin both directions: the analyzer must warn where a handle
leaks, and must stay quiet where it does not.

Usage: run_sys_thread_vm_tests.py [xray_binary]
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Sequence, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

# The two diagnostic wordings every leak case expects, spelled once.
NOT_CLOSED = ("Thread handle '{}' from sys.Thread.spawn is not joined or "
              "detached before leaving scope")
NEVER_USED = "Thread handle '{}' from sys.Thread.spawn is never used"
EXPLICIT = ("sys.Thread.spawn returns a Thread handle; call join() or detach() "
            "explicitly")

THREAD_SPAWN_OP = re.compile(r"^[0-9]+.*\sTHREAD_SPAWN\s", re.MULTILINE)


@dataclass(frozen=True)
class Case:
    name: str
    fixture: str
    expected: str
    # Only the yieldable case pins a worker count: it must make progress with a
    # single worker, which is where a missing yield would deadlock.
    workers: "int | None" = None
    # Non-empty for WARNING_CASES: every one must appear in stderr.
    needles: Tuple[str, ...] = ()


OUTPUT_CASES: Tuple[Case, ...] = (
    Case("spawn_join", "sys_thread_spawn_join", "42"),
    Case("join_array", "sys_thread_join_array", "42"),
    Case("spawn_options", "sys_thread_spawn_options", "42"),
    Case("alias_join", "sys_thread_alias_join", "42"),
    Case("return_transfer", "sys_thread_return_transfer", "42"),
    Case("branch_both_close", "sys_thread_branch_both_close", "42"),
    Case("branch_alias_both_close", "sys_thread_branch_alias_both_close", "42"),
    Case("try_catch_both_close", "sys_thread_try_catch_both_close", "42"),
    Case("match_all_close", "sys_thread_match_all_close", "42"),
    Case("select_all_close", "sys_thread_select_all_close", "42"),
    Case("ternary_both_close", "sys_thread_ternary_both_close", "42"),
    Case("destructure_join", "sys_thread_destructure_join", "42"),
    Case("destructure_alias_join", "sys_thread_destructure_alias_join", "42"),
    Case("destructure_helper_return_alias_join",
         "sys_thread_lifecycle_destructure_helper_return_alias_join", "42"),
    Case("destructure_factory_return_join",
         "sys_thread_lifecycle_destructure_factory_return_join", "42"),
    Case("assignment_alias_join", "sys_thread_assignment_alias_join", "42"),
    Case("loop_join", "sys_thread_loop_join", "42"),
    Case("const_true_loop_join", "sys_thread_lifecycle_const_true_loop_join", "42"),
    Case("for_loop_join", "sys_thread_for_loop_join", "42"),
    Case("loop_nested_continue_join", "sys_thread_loop_nested_continue_join", "42"),
    Case("template_join", "sys_thread_template_join", "joined 42"),
    Case("slice_bound_join", "sys_thread_slice_bound_join", "2\n20"),
    Case("channel_capacity_join", "sys_thread_channel_capacity_join", "channel"),
    Case("unsafe_join", "sys_thread_unsafe_join", "42"),
    Case("threadlocal_basic", "sys_threadlocal_basic", "10\n20\n15\n20"),
    Case("yieldable_join", "sys_thread_yieldable_join",
         "tick\n1\njoined 7\n7", workers=1),
    Case("defer_join", "sys_thread_defer_join", "thread-defer\n42"),
    Case("helper_join", "sys_thread_lifecycle_helper_join", "helper joined 42\n42"),
    Case("fn_value_join", "sys_thread_lifecycle_fn_value_join",
         "fn-value joined 42\n42"),
    Case("top_const_fn_value_join", "sys_thread_lifecycle_top_const_fn_value_join",
         "top-fn-value joined 42\n42"),
    Case("helper_return_alias_join",
         "sys_thread_lifecycle_helper_return_alias_join", "42"),
    Case("nested_helper_return_alias_join",
         "sys_thread_lifecycle_nested_helper_return_alias_join", "42"),
    Case("ternary_helper_return_alias_join",
         "sys_thread_lifecycle_ternary_helper_return_alias_join", "42"),
    Case("nullish_helper_return_alias_join",
         "sys_thread_lifecycle_nullish_helper_return_alias_join", "42"),
    Case("match_helper_return_alias_join",
         "sys_thread_lifecycle_match_helper_return_alias_join", "42"),
    Case("helper_match_return_alias_join",
         "sys_thread_lifecycle_helper_match_return_alias_join", "42"),
    Case("move_alias_join", "sys_thread_lifecycle_move_alias_join", "42"),
    Case("helper_arg_alias_wrappers_join",
         "sys_thread_lifecycle_helper_arg_alias_wrappers_join", "42\n42"),
    Case("unsafe_helper_return_alias_join",
         "sys_thread_lifecycle_unsafe_helper_return_alias_join", "42"),
    Case("unsafe_return_alias_join",
         "sys_thread_lifecycle_unsafe_return_alias_join", "42"),
    Case("helper_chained_return_alias_join",
         "sys_thread_lifecycle_helper_chained_return_alias_join", "42"),
    Case("helper_forward_return_alias_join",
         "sys_thread_lifecycle_helper_forward_return_alias_join", "42"),
    Case("helper_direct_return_alias_join",
         "sys_thread_lifecycle_helper_direct_return_alias_join", "42"),
    Case("helper_factory_return_join",
         "sys_thread_lifecycle_helper_factory_return_join", "42"),
    Case("helper_finalizer_return_arg_join",
         "sys_thread_lifecycle_helper_finalizer_return_arg_join", "42"),
    Case("const_alias_return_receiver_join",
         "sys_thread_lifecycle_const_alias_return_receiver_join", "42"),
    Case("top_const_alias_return_receiver_join",
         "sys_thread_lifecycle_top_const_alias_return_receiver_join", "42"),
    Case("for_in_literal_join", "sys_thread_lifecycle_for_in_literal_join", "42"),
    Case("for_in_const_literal_join",
         "sys_thread_lifecycle_for_in_const_literal_join", "42"),
    Case("for_in_const_range_join",
         "sys_thread_lifecycle_for_in_const_range_join", "42"),
)

WARNING_CASES: Tuple[Case, ...] = (
    Case("orphan", "sys_thread_orphan_warning", "orphan", needles=(EXPLICIT,)),
    Case("unused_local", "sys_thread_unused_local_warning", "unused-local",
         needles=(NEVER_USED.format("t"), "call join() or detach() explicitly")),
    Case("done_warning", "sys_thread_lifecycle_done_warning", "done-check",
         needles=(NOT_CLOSED.format("t"),)),
    Case("branch_join_warning", "sys_thread_branch_join_warning",
         "conditional-join", needles=(NOT_CLOSED.format("t"),)),
    Case("try_catch_warning", "sys_thread_try_catch_warning",
         "42\ntry-catch-open", needles=(NOT_CLOSED.format("t"),)),
    Case("match_warning", "sys_thread_match_warning", "match-open\nafter-match",
         needles=(NOT_CLOSED.format("t"),)),
    Case("select_warning", "sys_thread_select_warning",
         "select-open\nafter-select", needles=(NOT_CLOSED.format("t"),)),
    Case("ternary_warning", "sys_thread_ternary_warning", "0\nafter-ternary",
         needles=(NOT_CLOSED.format("t"),)),
    Case("loop_join_warning", "sys_thread_loop_join_warning", "conditional-loop",
         needles=(NOT_CLOSED.format("t"),)),
    Case("const_dynamic_loop_warning",
         "sys_thread_lifecycle_const_dynamic_loop_warning", "const-dynamic-loop",
         needles=(NOT_CLOSED.format("t"),)),
    Case("loop_continue_warning", "sys_thread_loop_continue_warning", "42",
         needles=(NOT_CLOSED.format("t"),)),
    Case("loop_try_continue_warning", "sys_thread_loop_try_continue_warning", "42",
         needles=(NOT_CLOSED.format("t"),)),
    Case("for_in_spread_warning", "sys_thread_lifecycle_for_in_spread_warning",
         "spread-empty", needles=(NOT_CLOSED.format("t"),)),
    Case("for_in_const_spread_warning",
         "sys_thread_lifecycle_for_in_const_spread_warning", "const-spread-empty",
         needles=(NOT_CLOSED.format("t"),)),
    Case("move_alias_warning", "sys_thread_lifecycle_move_alias_warning", "moved",
         needles=(NOT_CLOSED.format("t"),)),
    Case("reassigned_alias_warning",
         "sys_thread_lifecycle_reassigned_alias_warning", "2\n4",
         needles=(NOT_CLOSED.format("original"), NOT_CLOSED.format("first"))),
    Case("branch_alias_warning", "sys_thread_lifecycle_branch_alias_warning", "2",
         needles=(NOT_CLOSED.format("leaked"),)),
    Case("branch_alias_merge_warning",
         "sys_thread_lifecycle_branch_alias_merge_warning", "7",
         needles=(NOT_CLOSED.format("other"),)),
    Case("multipath_alias_merge_warning",
         "sys_thread_lifecycle_multipath_alias_merge_warning", "1\n3\n5",
         needles=(NOT_CLOSED.format("oldTry"), NOT_CLOSED.format("oldMatch"),
                  NOT_CLOSED.format("oldSelect"))),
    Case("helper_early_return_warning",
         "sys_thread_lifecycle_helper_early_return_warning", "helper-skip",
         needles=(NOT_CLOSED.format("t"),)),
    Case("helper_control_exit_warning",
         "sys_thread_lifecycle_helper_control_exit_warning",
         "try-after\nmatch-after",
         needles=(NOT_CLOSED.format("t0"), NOT_CLOSED.format("t1"))),
    Case("helper_factory_return_warning",
         "sys_thread_lifecycle_helper_factory_return_warning",
         "helper-factory-thread", needles=(NOT_CLOSED.format("t"),)),
    Case("helper_factory_discard_warning",
         "sys_thread_lifecycle_helper_factory_discard_warning",
         "helper-factory-discard-thread", needles=(EXPLICIT,)),
    Case("destructure_factory_return_warning",
         "sys_thread_lifecycle_destructure_factory_return_warning",
         "destructure-factory-thread", needles=(NOT_CLOSED.format("t"),)),
    Case("conditional_factory_warning",
         "sys_thread_lifecycle_conditional_factory_warning",
         "conditional-factory-thread", needles=(NEVER_USED.format("t"),)),
    Case("match_factory_warning", "sys_thread_lifecycle_match_factory_warning",
         "match-factory-thread", needles=(NEVER_USED.format("t"),)),
)

# Detaching is a fire-and-forget path with no join to synchronise on, so a
# single green run proves little; repeats are what surface a flaky teardown.
DETACH_REPEATS = 10


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, stdout: str, stderr: str,
            stderr_lines: int = 40) -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        for line in stdout.splitlines()[:40]:
            print(f"      stdout: {line}")
        for line in stderr.splitlines()[:stderr_lines]:
            print(f"      stderr: {line}")


def run_case(xray: Path, case: Case, timeout: "float | None"):
    argv: List = [xray, "run"]
    if case.workers is not None:
        argv += ["--workers", str(case.workers)]
    argv.append(PROJECT_DIR / "tests" / "vm" / f"{case.fixture}.xr")
    return proc.run(argv, timeout=timeout)


def check_output(rec: Recorder, xray: Path, case: Case, label: str,
                 timeout: "float | None") -> None:
    result = run_case(xray, case, timeout)
    stdout = result.stdout.decode("utf-8", "replace")
    stderr = result.stderr.decode("utf-8", "replace")
    if result.ok and stdout.rstrip("\n") == case.expected and not stderr:
        rec.ok(f"{label} output")
    else:
        rec.bad(f"{label} output", stdout, stderr)


def check_warning(rec: Recorder, xray: Path, case: Case,
                  timeout: "float | None") -> None:
    result = run_case(xray, case, timeout)
    stdout = result.stdout.decode("utf-8", "replace")
    stderr = result.stderr.decode("utf-8", "replace")
    missing = any(needle not in stderr for needle in case.needles)
    if result.ok and stdout.rstrip("\n") == case.expected and not missing:
        rec.ok(f"{case.name} warning")
    else:
        rec.bad(f"{case.name} warning", stdout, stderr, stderr_lines=80)


def main(argv: List[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))

    print("=== VM sys.Thread Tests ===")
    print(f"Binary: {xray}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    rec = Recorder()
    detach = Case("detach", "sys_thread_detach", "detached")

    check_output(rec, xray, OUTPUT_CASES[0], OUTPUT_CASES[0].name, timeout)
    for i in range(1, DETACH_REPEATS + 1):
        check_output(rec, xray, detach, f"detach_{i}", timeout)
    for case in OUTPUT_CASES[1:]:
        check_output(rec, xray, case, case.name, timeout)
    for case in WARNING_CASES:
        check_warning(rec, xray, case, timeout)

    # The handle must lower to the dedicated opcode, not to a generic call:
    # THREAD_SPAWN is what the lifecycle analysis keys on.
    dump = proc.run([xray, "run", "--dump-bytecode",
                     PROJECT_DIR / "tests" / "vm" / "sys_thread_spawn_join.xr"],
                    timeout=timeout)
    dump_out = dump.stdout.decode("utf-8", "replace")
    if THREAD_SPAWN_OP.search(dump_out):
        rec.ok("spawn_join bytecode uses THREAD_SPAWN")
    else:
        print("  FAIL: spawn_join bytecode uses THREAD_SPAWN")
        rec.failed += 1
        for line in dump_out.splitlines()[:120]:
            print(f"      {line}")
        for line in dump.stderr.decode("utf-8", "replace").splitlines()[:40]:
            print(f"      stderr: {line}")

    print("")
    print(f"Passed: {rec.passed}")
    print(f"Failed: {rec.failed}")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
