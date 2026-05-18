#!/usr/bin/env python3
"""Migrate .xr source files to the unified arrow syntax (task 082).

Transformations (applied in order):
  1.  `{ k => v }` and `#{ k => v }`  → `#{ k: v }` (Map literal, `#` prefix + `:`)
  2.  `(x: T): R => { ... }`           → `fn(x: T) -> R { ... }` (arrow closure with explicit return type, block body)
  3.  `(x: T): R => expr`              → `fn(x: T) -> R { return expr }` (single-expr form; conservative — only on a single line)
  4.  All remaining `=>`               → `->`
  5.  Function return type `): T {`    → `) -> T {`
  6.  Function return type `): T` at signature end (interface/abstract, no body) → `) -> T`
  7.  Function type `fn(T1, T2): R`    → `(T1, T2) -> R` (drop `fn` prefix in type position)

The script is token-naive (regex-based) but tries to skip:
  - String literals (single, double, template, raw)
  - Line comments (//) and block comments (/* */)
  - Byte-by-byte scan with simple state machine

Run:  python3 scripts/migrate_arrow_xr.py <file1.xr> <file2.xr> ...
Or:   find tests/regression -name '*.xr' -print0 | xargs -0 python3 scripts/migrate_arrow_xr.py
"""
import re
import sys
import pathlib


PLACEHOLDER_FMT = "__XR_SKIP_{}__"


def mask_skip_zones(src: str) -> tuple[str, list[str]]:
    """Replace string literals and comments with opaque placeholders so the
    main regex engine sees them as identifier-like tokens (no spaces, no
    arrows). The original text is recovered after all migrations finish.

    Returns (masked_text, list_of_originals_indexed_by_placeholder_index).
    """
    out = []
    originals: list[str] = []
    i = 0
    n = len(src)

    def push_skip(start: int, end: int) -> None:
        idx = len(originals)
        originals.append(src[start:end])
        out.append(PLACEHOLDER_FMT.format(idx))

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        # Line comment
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            if j < 0:
                j = n
            push_skip(i, j)
            i = j
            continue
        # Block comment
        if c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            if j < 0:
                j = n
            else:
                j += 2
            push_skip(i, j)
            i = j
            continue
        # String literal (double quote)
        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    break
                if src[j] == "\n":
                    break
                j += 1
            push_skip(i, j)
            i = j
            continue
        # String literal (single quote)
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    break
                if src[j] == "\n":
                    break
                j += 1
            push_skip(i, j)
            i = j
            continue
        # Template / raw string (`...`)
        if c == "`":
            j = i + 1
            while j < n and src[j] != "`":
                j += 1
            if j < n:
                j += 1
            push_skip(i, j)
            i = j
            continue
        out.append(c)
        i += 1

    return "".join(out), originals


def unmask_skip_zones(masked: str, originals: list[str]) -> str:
    # Restore in reverse order to avoid placeholder collisions if any nested
    # form ever ends up matching a substring of another (defensive).
    out = masked
    for idx in range(len(originals) - 1, -1, -1):
        out = out.replace(PLACEHOLDER_FMT.format(idx), originals[idx])
    return out


# --- Step 1: Map literal rewrite ---
# Match `{ "string" => v, ... }` or `#{ "string" => v, ... }` and rewrite
# to `#{ "string": v, ... }`. We deliberately restrict keys to **string
# literals** so the rule cannot accidentally match `match` arms (where
# the LHS is typically an integer literal or an identifier pattern such
# as `Color.Red`). Map literals using identifier or numeric keys without
# the leading `#` prefix are uncommon in practice and rely on manual
# review during migration.
MAP_LITERAL_PATTERN = re.compile(
    r"(?P<prefix>#?)\{\s*"
    r"(?P<body>(?:\"[^\"]*\"|'[^']*')"
    r"\s*=>\s*[^{}\n]*?(?:,\s*"
    r"(?:\"[^\"]*\"|'[^']*')"
    r"\s*=>\s*[^{}\n]*?)*)\s*\}"
)


def rewrite_map_literals(code: str) -> str:
    def repl(m: re.Match) -> str:
        body = m.group("body")
        # Replace each `=>` with `:`
        body = re.sub(r"\s*=>\s*", ": ", body)
        return "#{" + body + "}"

    # Multi-pass until no more changes (handles nested forms, but our pattern
    # already excludes nested braces, so one pass is usually enough).
    prev = None
    while prev != code:
        prev = code
        code = MAP_LITERAL_PATTERN.sub(repl, code)
    return code


# --- Step 2 & 3: arrow closure with explicit return type ---
# Block body form `(params): RET => { ... }` — rewrite to fn form.
CLOSURE_EXPLICIT_RET_BLOCK = re.compile(
    r"\(([^()\n]*?)\)\s*:\s*([A-Za-z_][A-Za-z0-9_<>?\|, &\.]*?)\s*=>\s*\{",
)


def rewrite_closure_explicit_block(code: str) -> str:
    return CLOSURE_EXPLICIT_RET_BLOCK.sub(r"fn(\1) -> \2 {", code)


# Single-expression form `(params): RET => expr` until end-of-statement.
# Conservative: only on a single line, stops at first `,` not enclosed, `)`,
# `}`, newline, or `"` (the latter prevents this rule from chewing past a
# C string-literal boundary when applied via migrate_arrow_c.py).
CLOSURE_EXPLICIT_RET_EXPR = re.compile(
    r"\(([^()\n]*?)\)\s*:\s*([A-Za-z_][A-Za-z0-9_<>?\|, &\.]*?)\s*=>\s*([^\n,)}\]\"]+)",
)


def rewrite_closure_explicit_expr(code: str) -> str:
    return CLOSURE_EXPLICIT_RET_EXPR.sub(
        lambda m: f"fn({m.group(1)}) -> {m.group(2)} {{ return {m.group(3).rstrip()} }}",
        code,
    )


# --- Step 4: bare `=>` to `->` ---
def rewrite_bare_arrow(code: str) -> str:
    return code.replace("=>", "->")


# --- Step 5: function return type `): T {`  →  `) -> T {` ---
# Type MUST start with a letter or underscore. Starting with `(` would
# greedily eat across an outer paren / arrow boundary (e.g. parameter
# destructuring `(x, y): (int, int)`), so we forbid it here. Nested
# function-returning-function chains (`): fn(): int {`) are handled by
# iterating the rewrite to a fixed point; each pass peels off one level.
FN_RETURN_BLOCK = re.compile(
    r"\)\s*:\s*([A-Za-z_][^:{}\n]*?)\s*\{",
)


def rewrite_fn_return_block(code: str) -> str:
    # Unit-return special case `): () {` -> `) -> () {`.
    # `()` cannot match the general pattern (type must start with a letter)
    # because that would also greedily eat across an outer `(tuple_type)`.
    code = re.sub(r"\)\s*:\s*\(\s*\)\s*\{", r") -> () {", code)
    # Tuple-return special case `): (T1, T2) {` -> `) -> (T1, T2) {`.
    # Restricted to single-level tuples (no nested parens) so the rule
    # cannot accidentally cross an outer paren boundary, which would
    # also catch `(x, y): (int, int))` parameter-destructuring sites.
    code = re.sub(
        r"\)\s*:\s*(\([^()\n]*\))\s*\{",
        r") -> \1 {",
        code,
    )
    return FN_RETURN_BLOCK.sub(r") -> \1 {", code)


# --- Step 6: function return type `): T` at end of signature ---
# Matches `): T` where T ends at EOL / `,` / `)` / `}` / `=` / `//`. Type
# MUST start with a letter or underscore so that `: (expr)` ternary tails
# in surrounding C code (when this script runs on C unit-test files) do
# not get misclassified as a function return type. Nested function-return
# types like `): (int) -> R` are handled by FN_RETURN_BLOCK during the
# `{`-terminated pass.
FN_RETURN_SIG = re.compile(
    r"\)\s*:\s*([A-Za-z_][^:{}\n]*?)(?=\s*(?:$|//|,|\)|\}|=\s|=$))",
    re.MULTILINE,
)


def rewrite_fn_return_signature(code: str) -> str:
    return FN_RETURN_SIG.sub(lambda m: f") -> {m.group(1)}", code)


# --- Step 7: `fn(T1, T2): R` function type → `(T1, T2) -> R` ---
FN_TYPE_PATTERN = re.compile(
    r"\bfn\(([^()\n]*?)\)\s*:\s*([A-Za-z_][A-Za-z0-9_<>?\|, &\.]*?)(?=\s*(?:$|//|,|\)|\}|=\s|=$|;))",
    re.MULTILINE,
)


def rewrite_fn_type(code: str) -> str:
    return FN_TYPE_PATTERN.sub(lambda m: f"({m.group(1)}) -> {m.group(2)}", code)


# --- Step 7a: bare `fn(T1, T2)` in type position WITHOUT return type
# (legacy form meaning a function returning unit) → `(T1, T2) -> ()`.
# Without this rule, the residual `fn`-prefix stripper below would turn
# `callback: fn(int)` into `callback: (int)`, which xray now reads as
# a single-element tuple type instead of a function type.
#
# We anchor in type position via the same lookbehind set used by 7b, and
# require that the `fn(...)` is NOT followed by `:` or `->` (which would
# mean a return type is coming and the form is already handled).
FN_TYPE_VOID = re.compile(
    # Include `=` in the lookbehind to cover `type Action = fn(int)` (type
    # alias bound to a void-return function type). Exclude in the lookahead:
    #   - `{` so that `let x = fn() { ... }` (anonymous fn expression) is
    #     left alone — that's an expression, not a type.
    #   - `;` so that C call sites like `int r = fn();` (a stray `fn` C
    #     identifier in unit-test scaffolding) are not mistaken for a
    #     type-position fn literal.
    r"(?<=[:>,&=])(\s*)\bfn\(([^()\n]*?)\)(?!\s*(?::|->|\{|;))",
)


def rewrite_fn_type_void(code: str) -> str:
    return FN_TYPE_VOID.sub(lambda m: f"{m.group(1)}({m.group(2)}) -> ()", code)


# --- Step 7b: drop residual `fn` prefix on function-type annotations
# in type-context positions. After Step 4 some `fn(T): R` already became
# `fn(T) -> R` in earlier passes (or via mixed input). The unified syntax
# is `(T) -> R` — function type literals do not carry a leading `fn`.
#
# Guarded: only strip `fn` when it sits in front of a complete function-type
# literal `(T1, T2) -> R`. The lookahead must see a balanced (single-level)
# parameter list followed by `->`. Without this guard, anonymous fn
# expressions in expression positions (e.g. `[fn(x) { ... }, fn(y) { ... }]`)
# would be misidentified as type literals because their leading `fn(` shares
# the same prefix.
#
# We also restrict the lookbehind to type-context delimiters and explicitly
# exclude `=` so that `let f = fn(...) ...` (anonymous fn expression) is
# left alone.
FN_PREFIX_IN_TYPE_POS = re.compile(
    r"(?<=[:>,&])\s*\bfn(?=\([^()\n]*\)\s*->)",
)


def rewrite_fn_prefix_in_type_pos(code: str) -> str:
    return FN_PREFIX_IN_TYPE_POS.sub("", code)


def migrate_code(code: str) -> str:
    code = rewrite_map_literals(code)
    code = rewrite_closure_explicit_block(code)
    code = rewrite_closure_explicit_expr(code)
    code = rewrite_bare_arrow(code)
    # rewrite_fn_type first so nested `fn(T): R` becomes `(T) -> R`, which
    # also clears the leftover `:` inside any outer function return type so
    # the relaxed FN_RETURN_BLOCK can match it next.
    code = rewrite_fn_type(code)
    # Iterate fn_return / fn_type until fixed point (handles deeply nested
    # function-returning-function chains).
    for _ in range(4):
        prev = code
        code = rewrite_fn_return_block(code)
        code = rewrite_fn_return_signature(code)
        code = rewrite_fn_type(code)
        # Convert legacy void-return `fn(T)` to `(T) -> ()` BEFORE stripping
        # the residual `fn` prefix, otherwise `fn(int)` would become `(int)`,
        # which xray now parses as a single-element tuple type.
        code = rewrite_fn_type_void(code)
        code = rewrite_fn_prefix_in_type_pos(code)
        if code == prev:
            break
    return code


def migrate_file(path: pathlib.Path) -> tuple[bool, int]:
    """Returns (changed, before_arrow_count)."""
    text = path.read_text(encoding="utf-8")
    before_arrow = text.count("=>")
    masked, originals = mask_skip_zones(text)
    migrated = migrate_code(masked)
    new_text = unmask_skip_zones(migrated, originals)
    if new_text == text:
        return False, before_arrow
    path.write_text(new_text, encoding="utf-8")
    return True, before_arrow


def main():
    if len(sys.argv) < 2:
        print("Usage: migrate_arrow_xr.py <file.xr> [<file.xr> ...]", file=sys.stderr)
        sys.exit(2)
    total = 0
    changed = 0
    total_arrows = 0
    for path_str in sys.argv[1:]:
        p = pathlib.Path(path_str)
        if not p.is_file():
            print(f"  skip non-file: {p}", file=sys.stderr)
            continue
        total += 1
        was_changed, before_arrow = migrate_file(p)
        if was_changed:
            changed += 1
            total_arrows += before_arrow
            print(f"  migrated: {p}  (=> count {before_arrow})", file=sys.stderr)
    print(f"Done: {changed}/{total} files changed, {total_arrows} `=>` removed", file=sys.stderr)


if __name__ == "__main__":
    main()
