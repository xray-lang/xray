#!/usr/bin/env python3
"""Migrate emit_abc/emit_abx/emit_asbx call sites to xemit_<op>() typed API.

Rewrites patterns of the form

    emit_abc(<emitter>, OP_NAME, A, B, C)
    emit_abx(<emitter>, OP_NAME, A, Bx)
    emit_asbx(<emitter>, OP_NAME, A, sBx)
    EMIT_ABC(ctx, c, OP_NAME, A, B, C)
    EMIT_ABX(ctx, c, OP_NAME, A, Bx)
    EMIT_ASBX(ctx, c, OP_NAME, A, sBx)

into the corresponding strongly typed call

    xemit_<name>(<emitter>, ...kept_args...)

where `kept_args` is determined by the opcode's KOP_* tag in
xopcode_def.h. Slots tagged NONE / 0 in the typed signature are
dropped; remaining slots flow through unchanged.

Calls whose opcode argument is not a literal OP_<NAME> (e.g.
function parameters that hold an OpCode runtime value) are left
alone; those represent legitimate dynamic dispatch.

Usage:
    python3 scripts/migrate_xemit_typed.py FILE [FILE ...]
    python3 scripts/migrate_xemit_typed.py --dry-run FILE
    python3 scripts/migrate_xemit_typed.py --all   # migrate every codegen file

The script is idempotent: running twice on the same file is a no-op.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEF_FILE = REPO / "src/runtime/value/xopcode_def.h"
DEFAULT_ROOTS = [REPO / "src/frontend/codegen", REPO / "src/aot"]

# Per-KOP, which positions in the original (a, b, c) or (a, bx) call
# should survive into the typed call. None means "drop slot whose typed
# signature is the literal 0".
#
# Encoder is 'abc' for OP arg-positions [a, b, c] (length 3),
# 'abx' for [a, bx] (length 2), 'asbx' for [a, sbx] (length 2).
KOP_KEEP: dict[str, tuple[str, tuple[int, ...]]] = {
    # Encoder, indices to keep (0-based into the source-call args).
    "KOP_NONE":            ("abc",  ()),
    "KOP_A_LOAD":          ("abc",  (0,)),
    "KOP_A_USE":           ("abc",  (0,)),
    "KOP_A_INOUT":         ("abc",  (0,)),
    "KOP_A_LIT":           ("abc",  (0,)),
    "KOP_A_TEST":          ("abc",  (0, 2)),

    "KOP_AB_UNARY":        ("abc",  (0, 1)),
    "KOP_AB_INPLACE":      ("abc",  (0, 1)),
    "KOP_AB_INOUT_IN":     ("abc",  (0, 1)),
    "KOP_AB_NEW_LIT":      ("abc",  (0, 1)),
    "KOP_AB_BASE_LIT":     ("abc",  (0, 1)),
    "KOP_AB_RECV":         ("abc",  (0, 1)),
    "KOP_AB_K":            ("abc",  (0, 1)),
    "KOP_AB_FLAG":         ("abc",  (0, 1)),
    "KOP_AB_TEST":         ("abc",  (0, 1, 2)),
    "KOP_AB_TEST_K":       ("abc",  (0, 1, 2)),
    "KOP_AB_TEST_S":       ("abc",  (0, 1, 2)),

    "KOP_ABC_BIN":         ("abc",  (0, 1, 2)),
    "KOP_ABC_INPLACE":     ("abc",  (0, 1, 2)),
    "KOP_ABC_BIN_K":       ("abc",  (0, 1, 2)),
    "KOP_ABC_BIN_S":       ("abc",  (0, 1, 2)),
    "KOP_ABC_BIN_LIT":     ("abc",  (0, 1, 2)),
    "KOP_ABC_BIN_SYM":     ("abc",  (0, 1, 2)),
    "KOP_ABC_INPLACE_K":   ("abc",  (0, 1, 2)),
    "KOP_ABC_INPLACE_LIT": ("abc",  (0, 1, 2)),
    "KOP_ABC_INPLACE_SYM": ("abc",  (0, 1, 2)),

    "KOP_ABx_K":           ("abx",  (0, 1)),
    "KOP_AsBx_LITS":       ("asbx", (0, 1)),
    "KOP_PROTO":           ("abx",  (0, 1)),
    "KOP_GLOBAL_GET":      ("abx",  (0, 1)),
    "KOP_GLOBAL_SET":      ("abx",  (0, 1)),
    "KOP_ABx_LAYOUT":      ("abx",  (0, 1)),
    "KOP_ABx_LIT":         ("abx",  (0, 1)),

    "KOP_CALL":            ("abc",  (0, 1, 2)),
    "KOP_CALL_KEEP":       ("abc",  (0, 1, 2)),
    "KOP_RETURN":          ("abc",  (0, 1)),
    "KOP_INVOKE_K":        ("abc",  (0, 1, 2)),
    "KOP_INVOKE_SYM":      ("abc",  (0, 1, 2)),
    "KOP_INVOKE_DIRECT":   ("abc",  (0, 1, 2)),
    "KOP_INVOKE_BUILTIN":  ("abc",  (0, 1, 2)),

    "KOP_DUMP":            ("abc",  (0, 1)),
    "KOP_AB_UNARY_HINT":   ("abc",  (0, 1, 2)),
    "KOP_NEW_CONTAINER":   ("abc",  (0, 1, 2)),
    "KOP_PRINT":           ("abc",  (0, 1, 2)),

    "KOP_SPECIAL":         ("abc",  (0, 1, 2)),
}


# --- xopcode_def.h parsing ---------------------------------------------------

DEF_ENTRY_RE = re.compile(
    r"^\s*_\(\s*(?P<name>\w+)\s*,\s*(?P<fmt>FMT_\w+)\s*,\s*"
    r"(?P<kop>KOP_\w+)\s*,\s*\"[^\"]*\"\s*\)\s*\\?\s*$"
)


def load_opcode_kops() -> dict[str, str]:
    """OP_<NAME> -> KOP_*."""
    out: dict[str, str] = {}
    for line in DEF_FILE.read_text().splitlines():
        m = DEF_ENTRY_RE.match(line)
        if m:
            out[m.group("name")] = m.group("kop")
    if not out:
        raise SystemExit(f"could not parse opcodes from {DEF_FILE}")
    return out


# --- Bracket-balanced argument splitter --------------------------------------

def find_call_end(text: str, open_paren_idx: int) -> int:
    """Return the index of the matching ')' for '(' at open_paren_idx.
    Skips over nested () and respects "..." and '...' strings.
    Raises ValueError if unmatched."""
    assert text[open_paren_idx] == "("
    depth = 0
    i = open_paren_idx
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or ch == "'":
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 2
                    continue
                i += 1
            i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            # Line comment — skip to end of line.
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unmatched parenthesis")


def split_args(arg_text: str) -> list[str]:
    """Split 'a, b, c' respecting nested () and string literals."""
    args: list[str] = []
    depth = 0
    cur: list[str] = []
    i = 0
    n = len(arg_text)
    while i < n:
        ch = arg_text[i]
        if ch == '"' or ch == "'":
            quote = ch
            cur.append(ch)
            i += 1
            while i < n and arg_text[i] != quote:
                if arg_text[i] == "\\" and i + 1 < n:
                    cur.append(arg_text[i])
                    cur.append(arg_text[i + 1])
                    i += 2
                    continue
                cur.append(arg_text[i])
                i += 1
            if i < n:
                cur.append(arg_text[i])
                i += 1
            continue
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    if cur:
        args.append("".join(cur).strip())
    return args


# --- Migration core ----------------------------------------------------------

# Regex to find the start of any of the recognized helper calls. We match
# the function name + '(' so we can then bracket-balance from the '('.
CALL_HEAD_RE = re.compile(
    r"\b(?P<helper>emit_abc|emit_abx|emit_asbx|EMIT_ABC|EMIT_ABSC|EMIT_ABX|EMIT_ASBX)\s*\("
)

# An opcode literal looks like OP_FOO or OP_FOO_BAR (uppercase/underscore/digit).
OPCODE_LITERAL_RE = re.compile(r"^OP_[A-Z0-9_]+$")


class MigrationStats:
    def __init__(self) -> None:
        self.rewritten = 0
        self.skipped_dynamic = 0
        self.skipped_unknown = 0
        self.skipped_special_helper = 0
        self.mismatch_details: list[str] = []


def rewrite(text: str, opcode_kops: dict[str, str], stats: MigrationStats) -> str:
    """Return rewritten text with all migratable calls converted."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        m = CALL_HEAD_RE.search(text, i)
        if not m:
            out.append(text[i:])
            break
        # Copy everything up to the match.
        out.append(text[i:m.start()])
        helper = m.group("helper")
        open_paren = m.end() - 1  # position of '('
        try:
            close_paren = find_call_end(text, open_paren)
        except ValueError:
            # Bail out on malformed input.
            out.append(text[m.start():])
            break

        inner = text[open_paren + 1:close_paren]
        args = split_args(inner)

        # Determine layout: emit_*(emitter, op, ...rest)
        # vs              EMIT_*(ctx, c, op, ...rest)
        is_macro = helper.isupper()
        if is_macro:
            if len(args) < 3:
                # Not the shape we expect; leave alone.
                out.append(text[m.start():close_paren + 1])
                i = close_paren + 1
                continue
            ctx_arg, c_arg, op_arg = args[0], args[1], args[2]
            rest = args[3:]
            emitter_expr = f"({c_arg.strip()})->emitter"
            _ = ctx_arg  # Discarded; was only used to keep typed-checker happy.
        else:
            if len(args) < 2:
                out.append(text[m.start():close_paren + 1])
                i = close_paren + 1
                continue
            emitter_expr = args[0]
            op_arg = args[1]
            rest = args[2:]

        # Skip dynamic-opcode calls — keep them on emit_abc/abx/asbx.
        if not OPCODE_LITERAL_RE.match(op_arg.strip()):
            stats.skipped_dynamic += 1
            out.append(text[m.start():close_paren + 1])
            i = close_paren + 1
            continue

        op_name = op_arg.strip()[len("OP_"):]
        kop = opcode_kops.get(op_name)
        if kop is None or kop not in KOP_KEEP:
            stats.skipped_unknown += 1
            out.append(text[m.start():close_paren + 1])
            i = close_paren + 1
            continue

        # Verify the helper matches the encoder declared by KOP.
        kop_encoder, keep_indices = KOP_KEEP[kop]
        helper_encoder = {
            "emit_abc":  "abc",
            "emit_abx":  "abx",
            "emit_asbx": "asbx",
            "EMIT_ABC":  "abc",
            "EMIT_ABSC": "abc",
            "EMIT_ABX":  "abx",
            "EMIT_ASBX": "asbx",
        }[helper]
        # Encoder mismatch — typically an opcode whose KOP says ABC-shape
        # but a call site uses emit_abx (because the opcode is SPECIAL or
        # because the call predates the typed API). Leave the original
        # call alone: bit-equivalence is preserved either way and the
        # typed wrapper would have to silently re-pack Bx into B+C, which
        # changes parameter semantics in a way that isn't safe to do
        # automatically.
        if helper_encoder != kop_encoder:
            stats.skipped_special_helper += 1
            stats.mismatch_details.append(
                f"  {helper}({op_arg.strip()}, ...) but {kop} expects {kop_encoder}"
            )
            out.append(text[m.start():close_paren + 1])
            i = close_paren + 1
            continue

        expected_n = {"abc": 3, "abx": 2, "asbx": 2}[helper_encoder]
        if len(rest) != expected_n:
            stats.skipped_special_helper += 1
            stats.mismatch_details.append(
                f"  {helper}({op_arg.strip()}, ...) has {len(rest)} args after OP, expected {expected_n}"
            )
            out.append(text[m.start():close_paren + 1])
            i = close_paren + 1
            continue

        # EMIT_ABSC packs the C field as `(uint8_t)((sc) & 0xFF)`. We are
        # migrating to the typed API which signed-literal opcodes already
        # encode via XR_OPF_LIT_S; pass the raw value through.
        kept = [rest[idx] for idx in keep_indices]

        new_args = [emitter_expr] + kept
        new_call = f"xemit_{op_name.lower()}({', '.join(new_args)})"
        out.append(new_call)
        stats.rewritten += 1
        i = close_paren + 1

    return "".join(out)


# --- CLI ---------------------------------------------------------------------

def gather_default_files() -> list[Path]:
    files: list[Path] = []
    for root in DEFAULT_ROOTS:
        if not root.exists():
            continue
        for p in root.rglob("*.c"):
            files.append(p)
    return files


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*", help="Files to migrate")
    ap.add_argument("--all", action="store_true",
                    help="Migrate every codegen / aot .c file")
    ap.add_argument("--dry-run", action="store_true",
                    help="Show what would change, do not write files")
    args = ap.parse_args()

    if args.all:
        files = gather_default_files()
    elif args.files:
        files = [Path(p) for p in args.files]
    else:
        ap.error("specify FILES or --all")

    opcode_kops = load_opcode_kops()
    overall = MigrationStats()
    for path in files:
        text = path.read_text()
        stats = MigrationStats()
        new_text = rewrite(text, opcode_kops, stats)
        overall.rewritten += stats.rewritten
        overall.skipped_dynamic += stats.skipped_dynamic
        overall.skipped_unknown += stats.skipped_unknown
        overall.skipped_special_helper += stats.skipped_special_helper
        if new_text != text:
            verb = "would rewrite" if args.dry_run else "rewrote"
            print(f"{verb} {path}: +{stats.rewritten} typed call(s), "
                  f"skipped {stats.skipped_dynamic} dynamic / "
                  f"{stats.skipped_unknown} unknown / "
                  f"{stats.skipped_special_helper} encoder-mismatch")
            if not args.dry_run:
                path.write_text(new_text)
        if stats.mismatch_details:
            for detail in stats.mismatch_details:
                print(detail)
    print(
        f"\nTotal: +{overall.rewritten} typed calls, "
        f"{overall.skipped_dynamic} dynamic / "
        f"{overall.skipped_unknown} unknown / "
        f"{overall.skipped_special_helper} encoder-mismatch left."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
