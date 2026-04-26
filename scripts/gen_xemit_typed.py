#!/usr/bin/env python3
"""Generate src/frontend/codegen/xemit_typed.h from xopcode_def.h.

Each opcode entry in xopcode_def.h carries a KOP_* tag classifying
the runtime semantics of its A/B/C byte slots. This script reads
that table and emits one strongly typed `xemit_<op>()` inline
function per opcode whose signature reflects the KOP semantics —
parameter count, parameter names, and the choice of underlying
emit_abc / emit_abx / emit_asbx are all derived from the KOP tag,
not from per-opcode hand maintenance.

Adding a new opcode in xopcode_def.h means re-running this script;
no manual edit of the typed header is required.

Usage:
    python3 scripts/gen_xemit_typed.py [--check]

Without --check, writes src/frontend/codegen/xemit_typed.h. With
--check, prints whether the on-disk file is in sync and exits
non-zero if it isn't (suitable for CI).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEF_FILE = REPO_ROOT / "src/runtime/value/xopcode_def.h"
OUT_FILE = REPO_ROOT / "src/frontend/codegen/xemit_typed.h"

# KOP_NAME -> (params, encoder, args)
#   params  : list of (type, name) declaring the function parameters
#             in addition to the leading XrEmitter *e.
#   encoder : 'abc' / 'abx' / 'asbx' — which underlying emit helper.
#   args    : 3-tuple (a, b, c) or 2-tuple (a, bx) of expressions
#             passed to the encoder, using the parameter names.
#
# Rationale: the names here flow into the public API. They should
# describe the semantic role (dst / src / k_idx / sym_idx / ...)
# clearly so that call sites read like documentation. Slots tagged
# NONE expand to literal 0 in args.
KOP_SIGS: dict[str, tuple[list[tuple[str, str]], str, tuple[str, ...]]] = {
    # FMT_NONE / FMT_A — single-slot or no-slot opcodes.
    "KOP_NONE":          ([], "abc", ("0", "0", "0")),
    "KOP_A_LOAD":        ([("int", "dst")], "abc", ("dst", "0", "0")),
    "KOP_A_USE":         ([("int", "src")], "abc", ("src", "0", "0")),
    "KOP_A_INOUT":       ([("int", "reg")], "abc", ("reg", "0", "0")),
    "KOP_A_LIT":         ([("int", "lit")], "abc", ("lit", "0", "0")),
    "KOP_A_TEST":        ([("int", "cond_reg"), ("int", "k_flag")],
                          "abc", ("cond_reg", "0", "k_flag")),

    # FMT_AB / FMT_AB_IMM — two-slot opcodes.
    "KOP_AB_UNARY":      ([("int", "dst"), ("int", "src")],
                          "abc", ("dst", "src", "0")),
    "KOP_AB_INPLACE":    ([("int", "target"), ("int", "src")],
                          "abc", ("target", "src", "0")),
    "KOP_AB_INOUT_IN":   ([("int", "target"), ("int", "src")],
                          "abc", ("target", "src", "0")),
    "KOP_AB_NEW_LIT":    ([("int", "dst"), ("int", "storage")],
                          "abc", ("dst", "storage", "0")),
    "KOP_AB_BASE_LIT":   ([("int", "base"), ("int", "count")],
                          "abc", ("base", "count", "0")),
    "KOP_AB_RECV":       ([("int", "dst_base"), ("int", "src")],
                          "abc", ("dst_base", "src", "0")),
    "KOP_AB_K":          ([("int", "src"), ("int", "k_idx")],
                          "abc", ("src", "k_idx", "0")),
    "KOP_AB_FLAG":       ([("int", "src"), ("int", "k_flag")],
                          "abc", ("src", "k_flag", "0")),
    "KOP_AB_TEST":       ([("int", "lhs"), ("int", "rhs"), ("int", "k_flag")],
                          "abc", ("lhs", "rhs", "k_flag")),
    "KOP_AB_TEST_K":     ([("int", "lhs"), ("int", "k_idx"), ("int", "k_flag")],
                          "abc", ("lhs", "k_idx", "k_flag")),
    "KOP_AB_TEST_S":     ([("int", "lhs"), ("int", "sliteral"), ("int", "k_flag")],
                          "abc", ("lhs", "sliteral", "k_flag")),

    # FMT_ABC — three-slot opcodes.
    "KOP_ABC_BIN":       ([("int", "dst"), ("int", "lhs"), ("int", "rhs")],
                          "abc", ("dst", "lhs", "rhs")),
    "KOP_ABC_INPLACE":   ([("int", "target"), ("int", "key"), ("int", "value")],
                          "abc", ("target", "key", "value")),
    "KOP_ABC_BIN_K":     ([("int", "dst"), ("int", "src"), ("int", "k_idx")],
                          "abc", ("dst", "src", "k_idx")),
    "KOP_ABC_BIN_S":     ([("int", "dst"), ("int", "src"), ("int", "sliteral")],
                          "abc", ("dst", "src", "sliteral")),
    "KOP_ABC_BIN_LIT":   ([("int", "dst"), ("int", "src"), ("int", "literal")],
                          "abc", ("dst", "src", "literal")),
    "KOP_ABC_BIN_SYM":   ([("int", "dst"), ("int", "src"), ("int", "sym_idx")],
                          "abc", ("dst", "src", "sym_idx")),
    "KOP_ABC_INPLACE_K": ([("int", "target"), ("int", "k_idx"), ("int", "value")],
                          "abc", ("target", "k_idx", "value")),
    "KOP_ABC_INPLACE_LIT": ([("int", "target"), ("int", "literal"), ("int", "value")],
                            "abc", ("target", "literal", "value")),
    "KOP_ABC_INPLACE_SYM": ([("int", "target"), ("int", "sym_idx"), ("int", "value")],
                            "abc", ("target", "sym_idx", "value")),

    # FMT_ABx / FMT_AsBx / FMT_PROTO / FMT_GLOBAL — Bx-encoded opcodes.
    "KOP_ABx_K":         ([("int", "dst"), ("int", "k_idx")],
                          "abx", ("dst", "k_idx")),
    "KOP_AsBx_LITS":     ([("int", "dst"), ("int", "sbx")],
                          "asbx", ("dst", "sbx")),
    "KOP_PROTO":         ([("int", "dst"), ("int", "proto_idx")],
                          "abx", ("dst", "proto_idx")),
    "KOP_GLOBAL_GET":    ([("int", "dst"), ("int", "global_idx")],
                          "abx", ("dst", "global_idx")),
    "KOP_GLOBAL_SET":    ([("int", "src"), ("int", "global_idx")],
                          "abx", ("src", "global_idx")),
    "KOP_ABx_LAYOUT":    ([("int", "dst"), ("int", "layout_id")],
                          "abx", ("dst", "layout_id")),
    "KOP_ABx_LIT":       ([("int", "dst"), ("int", "value")],
                          "abx", ("dst", "value")),

    # Calls / invokes / opcode-specific shapes.
    "KOP_CALL":          ([("int", "base"), ("int", "nargs"), ("int", "nresults")],
                          "abc", ("base", "nargs", "nresults")),
    "KOP_CALL_KEEP":     ([("int", "base"), ("int", "nargs"), ("int", "keep_dst")],
                          "abc", ("base", "nargs", "keep_dst")),
    "KOP_RETURN":        ([("int", "base"), ("int", "nret")],
                          "abc", ("base", "nret", "0")),
    "KOP_INVOKE_K":      ([("int", "base"), ("int", "k_idx"), ("int", "nargs")],
                          "abc", ("base", "k_idx", "nargs")),
    "KOP_INVOKE_SYM":    ([("int", "base"), ("int", "sym_idx"), ("int", "nargs")],
                          "abc", ("base", "sym_idx", "nargs")),
    "KOP_INVOKE_DIRECT": ([("int", "base"), ("int", "recv_reg"), ("int", "method_slot")],
                          "abc", ("base", "recv_reg", "method_slot")),
    "KOP_INVOKE_BUILTIN": ([("int", "base"), ("int", "builtin_idx"), ("int", "nargs")],
                           "abc", ("base", "builtin_idx", "nargs")),

    # Builtin / debug / container ops with composite C field.
    "KOP_DUMP":          ([("int", "val_reg"), ("int", "indent")],
                          "abc", ("val_reg", "indent", "0")),
    "KOP_AB_UNARY_HINT": ([("int", "dst"), ("int", "src"), ("int", "slot_hint")],
                          "abc", ("dst", "src", "slot_hint")),
    "KOP_NEW_CONTAINER": ([("int", "dst"), ("int", "capacity"), ("int", "storage")],
                          "abc", ("dst", "capacity", "storage")),
    "KOP_PRINT":         ([("int", "val_reg"), ("int", "add_space"), ("int", "packed")],
                          "abc", ("val_reg", "add_space", "packed")),

    # Composite encodings without a clean per-slot meaning. Caller must
    # know what they are doing; signature stays generic int a/b/c.
    "KOP_SPECIAL":       ([("int", "a"), ("int", "b"), ("int", "c")],
                          "abc", ("a", "b", "c")),
}

ENTRY_RE = re.compile(
    r"_\(\s*(?P<name>\w+)\s*,\s*(?P<fmt>FMT_\w+)\s*,\s*"
    r"(?P<kop>KOP_\w+)\s*,\s*\"(?P<desc>[^\"]*)\"\s*\)",
    re.DOTALL,
)


def parse_opcodes(def_path: Path) -> list[tuple[str, str, str, str]]:
    """Return [(NAME, FMT, KOP, DESC), ...] in declaration order.

    xopcode_def.h is a `_(NAME, FMT, KOP, "desc") \\` table inside a
    macro list, so individual entries may span more than one source
    line via trailing-backslash continuations (SPAWN_CONT, NEW_STRUCT,
    SELECT_BLOCK, INST_TYPE_ARGS, ...). Splice the continuations away
    first, then run a single DOTALL regex scan so each opcode entry
    is matched whether or not it crosses line boundaries.
    """
    text = def_path.read_text()
    # Strip comments first so docstring placeholders such as
    # `_(NAME, FMT_TAG, KOP_TAG, "...")` cannot be matched as real
    # opcode entries.
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    spliced = re.sub(r"\\\s*\n\s*", " ", text)
    out = []
    for m in ENTRY_RE.finditer(spliced):
        out.append((m.group("name"), m.group("fmt"),
                    m.group("kop"), m.group("desc")))
    return out


HEADER_PROLOGUE = """\
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xemit_typed.h - Strongly typed per-opcode emitter API (generated).
 *
 * GENERATED FILE — DO NOT EDIT BY HAND.
 *
 *   Source of truth : src/runtime/value/xopcode_def.h
 *   Generator       : scripts/gen_xemit_typed.py
 *   Re-generate     : python3 scripts/gen_xemit_typed.py
 *
 * One inline function per VM opcode. The function signature mirrors
 * the KOP_* field-kind triple declared in xopcode_def.h, so the call
 * site documents — and the compiler enforces — which slot is a
 * register, a K-index, a symbol index, a literal flag, etc.
 *
 *   xemit_move(e, dst, src)           // OP_MOVE   : KOP_AB_UNARY
 *   xemit_dump(e, val_reg, indent)    // OP_DUMP   : KOP_DUMP
 *   xemit_add(e, dst, lhs, rhs)       // OP_ADD    : KOP_ABC_BIN
 *   xemit_invoke(e, base, sym_idx, nargs)  // OP_INVOKE : KOP_INVOKE_SYM
 *
 * The generic emit_abc / emit_abx / emit_asbx helpers in xemit.h
 * remain available for opcodes flagged KOP_SPECIAL or for emitter
 * internals such as peephole rewriting and patching.
 */

#ifndef XEMIT_TYPED_H
#define XEMIT_TYPED_H

// Intentionally NOT including xemit.h here: this header is included
// at the *end* of xemit.h so the inline bodies below see the already
// declared emit_abc / emit_abx / emit_asbx prototypes. Including
// xemit.h from here would create a chicken-and-egg cycle.

"""

HEADER_EPILOGUE = """\

#endif  // XEMIT_TYPED_H
"""


def emit_call(encoder: str, op: str, args: tuple[str, ...]) -> str:
    if encoder == "abc":
        a, b, c = args
        return f"emit_abc(e, OP_{op}, {a}, {b}, {c})"
    if encoder == "abx":
        a, bx = args
        return f"emit_abx(e, OP_{op}, {a}, {bx})"
    if encoder == "asbx":
        a, sbx = args
        return f"emit_asbx(e, OP_{op}, {a}, {sbx})"
    raise ValueError(f"unknown encoder {encoder!r}")


def render_function(name: str, fmt: str, kop: str, desc: str) -> str:
    sig = KOP_SIGS.get(kop)
    if sig is None:
        raise SystemExit(
            f"unknown KOP tag {kop!r} for opcode {name} — "
            f"add it to KOP_SIGS in {Path(__file__).name}"
        )
    params, encoder, args = sig
    param_decl = ", ".join(f"{ty} {nm}" for ty, nm in params)
    full_decl = "XrEmitter *e" if not param_decl else f"XrEmitter *e, {param_decl}"
    body = emit_call(encoder, name, args)
    func_name = f"xemit_{name.lower()}"
    # Compact two-line form: doc comment + one-line function body.
    # The whole block is wrapped in clang-format off/on (see
    # render_header) so the formatter cannot expand the body across
    # four lines, which would push this generated header past the
    # 800-line frontend cap (R10).
    return (
        f"// {fmt} / {kop} : {desc}\n"
        f"static inline int {func_name}({full_decl}) {{ return {body}; }}\n"
    )


def render_header(opcodes: list[tuple[str, str, str, str]]) -> str:
    parts = [HEADER_PROLOGUE, "// clang-format off\n\n"]
    for name, fmt, kop, desc in opcodes:
        parts.append(render_function(name, fmt, kop, desc))
    parts.append("\n// clang-format on\n")
    parts.append(HEADER_EPILOGUE)
    return "".join(parts)


def main() -> int:
    check = "--check" in sys.argv[1:]
    opcodes = parse_opcodes(DEF_FILE)
    if not opcodes:
        sys.stderr.write(f"no opcodes parsed from {DEF_FILE}\n")
        return 2
    rendered = render_header(opcodes)
    if check:
        if not OUT_FILE.exists():
            sys.stderr.write(f"{OUT_FILE} missing; run without --check\n")
            return 1
        existing = OUT_FILE.read_text()
        if existing != rendered:
            sys.stderr.write(
                f"{OUT_FILE} is out of sync with {DEF_FILE}; "
                f"re-run scripts/gen_xemit_typed.py\n"
            )
            return 1
        print(f"{OUT_FILE} in sync ({len(opcodes)} opcodes)")
        return 0
    OUT_FILE.write_text(rendered)
    print(f"wrote {OUT_FILE} ({len(opcodes)} opcodes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
