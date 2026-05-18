#!/usr/bin/env python3
"""Migrate xray syntax in design docs:
- => to -> in function/match/select/closure contexts
- ): T { to ) -> T { for function return types
- fn(...): T to (...) -> T for function type annotations
- { k => v } / #{ k => v } to #{ k: v } for Map literals
- (x): T => { ... } closure with explicit return -> rewrite to fn(x) -> T { ... }

Strategy:
- Only modify content above the v0.7.x changelog history (which describes
  historical fact and must remain verbatim).
- Use targeted regex; preserve formatting & line boundaries.
"""
import re
import sys
import pathlib

# Boundary: everything from this marker onward is preserved verbatim.
HISTORY_MARKER = "| v0.7.5 |"


def migrate_body(text: str) -> str:
    out = text

    # 1. Closure with explicit return type rewrite:
    #    (x: T): R => { ... }    -> fn(x: T) -> R { ... }
    #    (x: T): R => expr       -> fn(x: T) -> R { return expr }
    # Approach: match opening pattern only; tail handled by generic => below.
    # Because spec rewrites are mostly inside ```xray code blocks, this rewrite
    # is conservative — we don't auto-wrap single-expr forms; those need manual edits.
    # So only do the textual swap for block-form which is the dominant case.
    out = re.sub(
        r"\(([^()\n]*?)\)\s*:\s*([A-Za-z_][A-Za-z0-9_<>?, &\.]*?)\s*=>\s*\{",
        r"fn(\1) -> \2 {",
        out,
    )

    # 2a. Function return type with block body: `): T {`  ->  `) -> T {`
    out = re.sub(
        r"\)\s*:\s*([A-Za-z_][A-Za-z0-9_<>?\|, &\.]*?)\s*\{",
        r") -> \1 {",
        out,
    )

    # 2b. Function return type at end of signature (interface methods, type aliases,
    # comments referring to signatures): `): T` followed by EOL / `,` / `)` / `}` /
    # ` =` / ` //` comment / ` ->` (chained return like `): fn(): int`).
    # Stops T at the boundary token.
    # Guard: do NOT match in lines that start with `|` (markdown tables) or `>` (quote).
    def replace_return_type(match):
        prefix = match.group(1)
        ret_type = match.group(2)
        suffix = match.group(3)
        return f"{prefix}) -> {ret_type}{suffix}"

    # Run line by line so we can skip table rows safely.
    new_lines = []
    for line in out.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith("|"):
            # Markdown table row: handle inline `): T` carefully (only in `fn name(): T` form)
            line = re.sub(
                r"(\bfn\b[^()|]*\([^()|]*\))\s*:\s*([A-Za-z_][A-Za-z0-9_<>?\|, &\.]*?)(?=\s*[\|`,)])",
                r"\1 -> \2",
                line,
            )
        else:
            # General code/text: match `): T` where T is followed by clear terminator.
            line = re.sub(
                r"(\b\w+\s*\([^()\n]*\))\s*:\s*([A-Za-z_][A-Za-z0-9_<>?\|, &\.]*?)(?=\s*(?:$|//|,|\)|\}|->|=\s|=$))",
                lambda m: f"{m.group(1)} -> {m.group(2)}",
                line,
            )
        new_lines.append(line)
    out = "\n".join(new_lines)

    # 3. Function type annotation: `fn(T1, T2): R`  ->  `(T1, T2) -> R`
    # Only when fn( is immediately preceded by `:` (type position) or `=` or whitespace
    # but NOT after `fn name` (declaration). Anchor pattern: standalone `fn(...)`.
    # Use word boundary: only `fn(` not preceded by identifier-ish.
    out = re.sub(
        r"\bfn\(([^()\n]*?)\)\s*:\s*([A-Za-z_][A-Za-z0-9_<>?\|, &\.]*?)(?=\s*[\)\,\=\n\{>}])",
        r"(\1) -> \2",
        out,
    )

    # 4. Void fn type: `fn(T)` -> `(T) -> ()` (only when fn() has no return type
    # in type position, e.g. `type Action = fn(int)`).
    # Skip — manual review.

    # 5. Map literal `#{ "k" => v }`  ->  `#{ "k": v }`
    # Replace `=>` inside #{...} blocks. Multi-pass: scan for `#{` openings.
    def replace_map_arrow(match):
        body = match.group(1)
        # Replace => with : but only if it's a key-value separator (not nested).
        body = re.sub(r"\s*=>\s*", ": ", body)
        return "#{" + body + "}"

    # Simple non-nested form: #{...} on a single segment
    out = re.sub(r"#\{([^{}]*?)\}", replace_map_arrow, out)

    # 6. Map literal `{ "k" => v }` (no `#`)  ->  `#{ "k": v }`
    # ONLY when => is the entry separator, not arrow function body or match arm.
    # Heuristic: { "..." => ...} or { 'name' => ...} or { ident => ...} where
    # first key is a string/number/identifier followed by => and second is value.
    # We restrict to forms inside code blocks with `=>` joined to map-like keys.
    def replace_plain_map(match):
        body = match.group(1)
        # Has `=>` and looks like map (key on LHS is literal/identifier).
        # Replace only if not detected as match/select arm structure.
        # Keep as `#{...:...}`.
        new_body = re.sub(r"\s*=>\s*", ": ", body)
        return "#{" + new_body + "}"

    # Restrict pattern: { "string" => ... } or { identifier => ... } single line
    out = re.sub(
        r"\{\s*((?:\"[^\"]*\"|'[^']*'|[A-Za-z_][A-Za-z0-9_]*)\s*=>\s*[^{}\n]*?)\s*\}",
        replace_plain_map,
        out,
    )

    # 7. Generic `=>` -> `->` for match/select/closure arrows.
    # Now that map literals are handled, remaining `=>` should all become `->`.
    out = out.replace("=>", "->")

    # 8. select/match arm `:` description in keyword table:
    #    `select 的超时分支 (after 1000 -> ...)` — done by step 7.

    # 9. Fix EBNF arrow function rule which had both -> Type and => body:
    # OLD: ArrowFunction ::= '(' ArrowParams? ')' ('->' Type)? '=>' (Expression | Block)
    # NEW: ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)
    out = re.sub(
        r"ArrowFunction ::= '\(' ArrowParams\? '\)' \('->' Type\)\? '->' \(Expression \| Block\)",
        "ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)",
        out,
    )

    return out


def main():
    if len(sys.argv) < 2:
        print("Usage: migrate_arrow_doc.py <file.md> [<file.md> ...]", file=sys.stderr)
        sys.exit(2)

    for path_str in sys.argv[1:]:
        path = pathlib.Path(path_str)
        text = path.read_text(encoding="utf-8")
        idx = text.find(HISTORY_MARKER)
        if idx >= 0:
            body = text[:idx]
            history = text[idx:]
        else:
            body = text
            history = ""

        new_body = migrate_body(body)
        new_text = new_body + history

        if new_text == text:
            print(f"  {path}: unchanged", file=sys.stderr)
        else:
            path.write_text(new_text, encoding="utf-8")
            # Stats
            before_arrow = text.count("=>")
            after_arrow = new_text.count("=>")
            print(f"  {path}: => count {before_arrow} -> {after_arrow}", file=sys.stderr)


if __name__ == "__main__":
    main()
