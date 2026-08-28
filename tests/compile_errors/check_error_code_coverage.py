#!/usr/bin/env python3
"""Every error code the compiler can show a user must have a case that pins it.

An error code is a promise printed in front of a user: `error[E0359]`. Three
places have to agree about that promise, and nothing kept them in sync:

  A  spec/source/sections/019-18-error-codes.md   what the language documents
  B  src/runtime/xerror_codes.h                   what the compiler can emit
  C  tests/compile_errors/**/*.expected           what a case actually demands

A code in B with nothing in C is a diagnostic no test would notice losing. A
code in B missing from A is a message users meet with no documentation behind
it. This script prints all three differences and fails on the one that rots
silently: a code the compiler can emit at compile time that no case pins.

Coverage is read through `expected_format.parse_expected()` and counted only
from an assertion's `code` field -- never by grepping for `E[0-9]{4}`, which
would count a code that merely appears inside a message and credit coverage to
a case that never demanded it. `.expected-runtime` siblings are excluded for
the same reason: they declare that the compiler does NOT reject the case, so
they are the record of a gap, not coverage of one.

Most of B genuinely cannot be covered here. The runtime half of the code space
is thrown by the VM, the AOT runtime headers, and stdlib bindings, long after
compilation succeeded; a large dead half is defined in the header and emitted
by nobody. Those are listed in EXEMPT, each with the reason it cannot be a
compile-error case. The list is the honest inventory, not a mute button: an
entry is a claim about the compiler that a reader can check against `src/`,
and a code that gains a real emission point must lose its entry here.

Usage: check_error_code_coverage.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


def _bootstrap() -> None:
    here = str(Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)


_bootstrap()
from expected_format import ExpectedFormatError, parse_expected  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
SPEC_FILE = PROJECT_DIR / "spec" / "source" / "sections" / "019-18-error-codes.md"
HEADER_FILE = PROJECT_DIR / "src" / "runtime" / "xerror_codes.h"

# Spec tables open every row with the code in backticks: `| `E0101` | ... |`.
# The chapter carries a Chinese and an English copy of the same tables, so the
# codes are collected as a set and the duplication costs nothing.
SPEC_ROW_RE = re.compile(r"^\|\s*`(E[0-9]{4})`\s*\|", re.M)
# The header stores plain integers; `src/frontend/xdiag_fmt.h` renders them with
# `%04d` behind an `E`, which is the only place the user-visible spelling of a
# code is decided.
DEFINE_RE = re.compile(r"^#define\s+(XR_ERR_[A-Z0-9_]+)\s+([0-9]+)\s*$", re.M)

# Codes that exist in the header but cannot be demanded by a compile-error case,
# each with the reason. Two reasons dominate, both established by walking every
# emission point of every code in `src/`:
#
#   runtime  the only emission points run after compilation succeeded -- the VM,
#            the AOT runtime headers, the Xi lowering's generated traps, or a
#            stdlib binding. A case here would have to run the program, which is
#            precisely what this suite refuses to accept as a compile error.
#   dead     the header defines the code and nothing in the repository emits it:
#            no use of the macro, no `"E0xxx"` text spliced into a message.
#
# A representative emission point is named for every runtime entry so the claim
# stays checkable; a dead entry names what took the code's job instead.
EXEMPT = {
    # -- Lexer: dead. `TK_ERROR` tokens carry only `error_message`, a string;
    # the token struct has no code field at all (src/frontend/lexer/xlex.h:261),
    # and `XR_ERR_` appears zero times under src/frontend/lexer/.
    "E0101": "dead: lexer diagnostics carry a message string and no code",
    "E0102": "dead: lexer diagnostics carry a message string and no code",
    "E0103": "dead: lexer diagnostics carry a message string and no code",
    "E0104": "dead: lexer diagnostics carry a message string and no code",
    # -- Parser: dead. `xr_parser_error_at()` (src/frontend/parser/xparse.c:504)
    # passes a literal 0 as the code, and xdiag_fmt.h prints `error:` instead of
    # `error[Exxxx]:` whenever the code is not positive. Every generic parser
    # diagnostic in the compiler therefore reaches the user with no code.
    "E0201": "dead: xr_parser_error_at() hard-codes code 0 for all generic parser errors",
    "E0202": "dead: xr_parser_error_at() hard-codes code 0 for all generic parser errors",
    "E0203": "dead: xr_parser_error_at() hard-codes code 0 for all generic parser errors",
    "E0204": "dead: xr_parser_error_at() hard-codes code 0 for all generic parser errors",
    "E0205": "dead: xr_parser_error_at() hard-codes code 0 for all generic parser errors",
    "E0206": "dead: xr_parser_error_at() hard-codes code 0 for all generic parser errors",
    "E0207": "dead: xr_parser_error_at() hard-codes code 0 for all generic parser errors",
    # -- Compiler stage.
    "E0301": "dead: undefined names are reported by the analyzer as E0351",
    "E0303": "runtime: VM byte-slice read-only writes (src/vm/xvm_dispatch_collection.inc.c:75) "
             "and the trap AOT cgen writes into generated C; compile-time const assignment is "
             "E0353",
    "E0307": "dead: bytecode-compiler capacity limit, nothing checks it today",
    "E0308": "dead: bytecode-compiler capacity limit, nothing checks it today",
    "E0309": "dead: bytecode-compiler capacity limit, nothing checks it today",
    "E0310": "dead: bytecode-compiler capacity limit, nothing checks it today",
    "E0311": "dead: bytecode-compiler capacity limit, nothing checks it today",
    # -- Static type errors, superseded wholesale by the analyzer's own codes.
    "E0321": "dead: superseded by the analyzer's E0354",
    "E0322": "dead: superseded by the analyzer's E0352",
    "E0323": "dead: superseded by the analyzer's E0352",
    "E0324": "dead: superseded by the analyzer's E0352/E0356",
    # -- Runtime (E04xx): thrown once the program is already running.
    "E0400": "runtime: src/vm/xvm_parallel_ops.c:274, src/coro/xthread_obj.c:498",
    "E0401": "runtime: src/vm/xvm_props.c:96",
    "E0402": "runtime: src/vm/xvm_dispatch_collection.inc.c:553, src/aot/xrt_exception.h:371",
    "E0403": "runtime: src/vm/xvm_dispatch_call.inc.c:70",
    "E0404": "runtime: src/vm/xvm_dispatch_collection.inc.c:45 and 240-odd further VM sites",
    "E0405": "runtime: src/vm/xvm_dispatch_call.inc.c:180",
    "E0406": "runtime: src/aot/xrt_coll.h:6910, its only emission point",
    "E0410": "runtime: src/vm/xvm_props.c:167, src/aot/xrt_coll.h:6909",
    "E0411": "dead: a null index reaches the VM as E0402",
    "E0412": "dead: a null call reaches the VM as E0403",
    "E0413": "runtime: text-spliced by Xi lowering, src/ir/xi_lower_expr.c:10948",
    "E0420": "runtime: src/vm/xvm_dispatch_arith.inc.c:147, src/aot/xrt_arith.h:457",
    "E0421": "runtime: src/vm/xvm_dispatch_arith.inc.c:181, src/aot/xrt_arith.h:466",
    "E0422": "runtime: src/vm/xvm_dispatch_convert.inc.c:234, src/aot/xrt_coll.h:577",
    "E0430": "runtime: src/vm/xvm_dispatch_collection.inc.c:524, plus AOT bounds-check traps",
    "E0431": "runtime: src/vm/xvm_dispatch_collection.inc.c:2009, src/aot/xrt_coll.h:2255",
    "E0432": "runtime: src/runtime/object/xiterator.c:626, src/aot/xrt_gen_iter.c:73",
    "E0440": "runtime: src/vm/xvm_dispatch_helpers.h:258",
    "E0441": "runtime: src/vm/xvm_dispatch_collection.inc.c:921",
    "E0442": "runtime: text-spliced by Xi lowering, src/ir/xi_lower_stmt.c:1708",
    "E0443": "runtime: src/shared/xr_error_core.h:76, src/aot/xrt_exception.h:182",
    "E0444": "runtime: src/vm/xvm_dispatch_call.inc.c:192, src/aot/xrt_exception.h:246",
    "E0450": "runtime: src/vm/xvm_dispatch_call.inc.c:304",
    "E0451": "runtime: src/aot/xrt_coll.h:6900, src/aot/xrt_core_freestanding.h:1104",
    "E0460": "runtime: src/vm/xvm_coro_ops.c:581",
    "E0461": "runtime: src/vm/xvm_coro_ops.c:922, src/coro/xaot_coro.c:2395",
    "E0462": "runtime: src/vm/xvm_props.c:350, src/coro/xaot_coro.c:2304",
    "E0470": "dead: every JSON failure reports E0471",
    "E0471": "runtime: src/runtime/object/builtins/xjson_builtins.c:117",
    "E0475": "dead: no regex emission point anywhere in the repository",
    "E0476": "dead: no regex emission point anywhere in the repository",
    "E0480": "runtime: emitted only from a stdlib binding, stdlib/http2/http2_binding.c:33",
    # -- Module (E05xx).
    "E0501": "dead: module resolution reports E0502 or E0504 instead",
    "E0502": "runtime: src/vm/xvm_dispatch_module.inc.c:45",
    "E0503": "runtime: src/vm/xvm_props.c:118",
    # -- IO (E06xx): the whole section is unused, except one misuse.
    "E0601": "runtime: the only emission points are bigint allocation failures in "
             "src/aot/xrt_arith.h:485 and :507, which is a misuse of an IO code; the declared "
             "file-not-found meaning is emitted nowhere",
    "E0602": "dead: no IO emission point anywhere in the repository",
    "E0603": "dead: no IO emission point anywhere in the repository",
    "E0604": "dead: no IO emission point anywhere in the repository",
    # -- Coroutine (E07xx): the whole section is unused.
    "E0701": "dead: coroutine failures report E0400/E0460/E0461 instead",
    "E0702": "dead: coroutine failures report E0400/E0460/E0461 instead",
    "E0703": "dead: coroutine failures report E0400/E0460/E0461 instead",
    # -- Removed syntax (E08xx). The parser really does reject each of these
    # forms; only E0802 and E0804 reject them through a coded diagnostic.
    "E0801": "emitted with the code spliced into the message text -- `error: [E0801] ...` rather "
             "than `error[E0801]:` (src/frontend/parser/xparse_decl.c:1473) -- so no assertion "
             "can carry it as a code; syntax/e0801_multi_value_return.xr pins the wording and "
             "position instead, and gains the code the day the emission does",
    "E0803": "dead: the removed for-form is rejected by an uncoded generic parser error",
    "E0805": "dead: the removed param mode is rejected by an uncoded generic parser error",
    "E0806": "dead: the removed param mode is rejected by an uncoded generic parser error",
    "E0807": "dead: the removed param mode is rejected by an uncoded generic parser error",
    "E0808": "dead: the removed param mode is rejected by an uncoded generic parser error",
    "E0809": "dead: the removed call-in marker is rejected by an uncoded generic parser error",
    # -- Internal (E09xx).
    "E0900": "runtime: src/vm/xvm_dispatch_convert.inc.c:200, src/aot/xrt_method.h:208",
    "E0901": "dead: no emission point anywhere in the repository",
    "E0999": "runtime: src/runtime/object/xpanic_info.c:131",
}


def spec_codes() -> set[str]:
    """Codes documented in the error-code chapter of the spec."""
    if not SPEC_FILE.is_file():
        raise FileNotFoundError(SPEC_FILE)
    return set(SPEC_ROW_RE.findall(SPEC_FILE.read_text(encoding="utf-8")))


def header_codes() -> dict[str, str]:
    """Codes the compiler can emit, mapped to the C name that defines them."""
    if not HEADER_FILE.is_file():
        raise FileNotFoundError(HEADER_FILE)
    text = HEADER_FILE.read_text(encoding="utf-8")
    return {f"E{int(value):04d}": name for name, value in DEFINE_RE.findall(text)}


def asserted_codes() -> tuple[dict[str, list[str]], list[str]]:
    """Codes demanded by cases, mapped to the expected files demanding them.

    An expected file that does not parse is reported rather than skipped: its
    codes would silently drop out of the coverage set and reappear as a gap
    somebody would then be asked to close twice.
    """
    found: dict[str, list[str]] = {}
    unreadable: list[str] = []
    for path in sorted(SCRIPT_DIR.rglob("*.expected")):
        try:
            assertions = parse_expected(path)
        except ExpectedFormatError as bad:
            unreadable.append(str(bad))
            continue
        for assertion in assertions:
            if assertion.code:
                found.setdefault(assertion.code, []).append(
                    str(path.relative_to(SCRIPT_DIR)))
    return found, unreadable


def _print_section(title: str, lines: list[str]) -> None:
    print("")
    print(title)
    print("-" * len(title))
    if not lines:
        print("  (none)")
        return
    for line in lines:
        print(f"  {line}")


def main(argv: list[str]) -> int:
    del argv  # No options: the three sources are fixed paths in this repository.

    spec = spec_codes()
    header = header_codes()
    covered, unreadable = asserted_codes()

    print("Error code coverage")
    print("========================================")
    print(f"A  spec table            {len(spec):4d}  {SPEC_FILE.relative_to(PROJECT_DIR)}")
    print(f"B  header definitions    {len(header):4d}  {HEADER_FILE.relative_to(PROJECT_DIR)}")
    print(f"C  asserted by cases     {len(covered):4d}  "
          f"{SCRIPT_DIR.relative_to(PROJECT_DIR)}/**/*.expected")

    _print_section(
        "Documented but not defined (spec has it, the compiler cannot emit it)",
        sorted(spec - set(header)))
    _print_section(
        "Defined but not documented (users meet it, the spec never explains it)",
        [f"{code}  {header[code]}" for code in sorted(set(header) - spec)])

    uncovered = sorted(set(header) - set(covered))
    _print_section(
        f"Defined but never asserted by a case ({len(uncovered)})",
        [f"{code}  {header[code]}"
         + ("" if code in EXEMPT else "   <-- NOT EXEMPT")
         for code in uncovered])

    # A code asserted by a case but absent from the header is a typo in an
    # expected file: the case demands something the compiler can never print.
    invented = sorted(set(covered) - set(header))
    stale_definition = sorted(code for code in EXEMPT if code not in header)
    now_covered = sorted(code for code in EXEMPT if code in covered)

    failures: list[str] = []
    for detail in unreadable:
        failures.append(f"expected file does not parse, so its codes cannot be counted: {detail}")
    for code in uncovered:
        if code not in EXEMPT:
            failures.append(
                f"{code} ({header[code]}) is defined and no case asserts it. Add a case under "
                f"{SCRIPT_DIR.name}/, or add an EXEMPT entry saying why it cannot have one.")
    for code in invented:
        failures.append(
            f"{code} is asserted by {', '.join(covered[code])} but no such code is defined in "
            f"{HEADER_FILE.name}")
    for code in stale_definition:
        failures.append(
            f"{code} is exempted but no longer defined in {HEADER_FILE.name}; drop its EXEMPT "
            "entry")

    if now_covered:
        # Good news, not a failure: a code left the exempt half by growing a
        # real compile-time diagnostic. Say so loudly enough that the stale
        # reason gets deleted rather than left to mislead the next reader.
        _print_section(
            "Exempt but now covered -- delete these EXEMPT entries",
            [f"{code}  ({EXEMPT[code]})" for code in now_covered])

    print("")
    print("========================================")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print(f"PASS: {len(header)} defined codes -- {len(covered)} asserted by cases, "
          f"{len(uncovered)} exempt with a stated reason")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
