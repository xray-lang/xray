#!/usr/bin/env python3
"""Gate the language spec's ```xray fences against the real compiler.

Every fenced block is classified before it is judged:

  runnable   -- self-contained (has a top-level main() call), no ellipsis
                placeholders, not marked as an intentional error: must pass
                `xray check`.
  error-demo -- the block's comments announce a compile error (编译错误 /
                error[Exxxx]): must FAIL `xray check`; a demo that silently
                passes is stale.
  fragment   -- everything else: skipped.

A baseline file lists snippet keys that are known-bad today. The gate only
fails on NEW deviations, so the spec can converge incrementally; fixing a
baselined snippet and leaving it in the baseline is reported as stale.
"""

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ELLIPSIS_RE = re.compile(r"(^|[^.])\.\.\.($|[^.])")
ERROR_MARK_RE = re.compile(r"编译错误|error\[E\d{4}\]|// error:")
MAIN_CALL_RE = re.compile(r"^main\(\)\s*$", re.M)


def extract_blocks(spec_text: str):
    blocks = []
    fence = None
    start_line = 0
    buf = []
    for i, line in enumerate(spec_text.splitlines(), 1):
        if fence is None:
            if line.strip().startswith("```xray"):
                fence = line.strip()
                start_line = i + 1
                buf = []
        else:
            if line.strip() == "```":
                blocks.append((start_line, "\n".join(buf)))
                fence = None
            else:
                buf.append(line)
    return blocks


def classify(code: str) -> str:
    if ERROR_MARK_RE.search(code):
        return "error-demo"
    if ELLIPSIS_RE.search(code):
        return "fragment"
    if not MAIN_CALL_RE.search(code):
        return "fragment"
    return "runnable"


def snippet_key(start_line: int, code: str) -> str:
    # Content-only identity: spec edits shift line numbers constantly, and a
    # baseline that rots on every unrelated edit is worse than none.
    del start_line
    digest = hashlib.sha256(code.encode("utf-8")).hexdigest()[:12]
    return f"sha:{digest}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--xray", required=True)
    ap.add_argument("--spec", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    spec_text = Path(args.spec).read_text(encoding="utf-8")
    baseline_path = Path(args.baseline)
    baseline = set()
    if baseline_path.is_file():
        for raw in baseline_path.read_text(encoding="utf-8").splitlines():
            line = raw.split("#", 1)[0].strip()
            if line:
                baseline.add(line.split()[0])

    deviations = []  # (key, line, kind, detail)
    passes_in_baseline = []
    counts = {"runnable": 0, "error-demo": 0, "fragment": 0}

    with tempfile.TemporaryDirectory(prefix="spec_snippets.") as tmp:
        for start_line, code in extract_blocks(spec_text):
            kind = classify(code)
            counts[kind] += 1
            if kind == "fragment":
                continue
            key = snippet_key(start_line, code)
            src = Path(tmp) / f"snippet_{start_line}.xr"
            src.write_text(code + "\n", encoding="utf-8")
            proc = subprocess.run(
                [args.xray, "check", str(src)],
                capture_output=True,
                encoding="utf-8",
                errors="strict",
                timeout=30,
            )
            ok = proc.returncode == 0
            expected_ok = kind == "runnable"
            if ok == expected_ok:
                if key in baseline:
                    passes_in_baseline.append((key, start_line))
                continue
            detail = (proc.stderr or proc.stdout).strip().splitlines()
            deviations.append((key, start_line, kind, detail[0] if detail else "<no output>"))

    if args.update_baseline:
        lines = [
            "# Known-deviating spec snippets: sha:<sha12> of the fence body per row.",
            "# Rows are removed as the spec or the compiler converges;",
            "# regenerate with --update-baseline only alongside a written reason.",
        ]
        for key, line, kind, detail in sorted(deviations, key=lambda d: d[1]):
            lines.append(f"{key}  # near line {line}, {kind}: {detail[:100]}")
        baseline_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"baseline rewritten with {len(deviations)} entries")
        return 0

    new = [(k, l, kind, d) for (k, l, kind, d) in deviations if k not in baseline]
    print(
        f"spec snippets: {counts['runnable']} runnable, {counts['error-demo']} error demos, "
        f"{counts['fragment']} fragments; {len(deviations)} deviations "
        f"({len(new)} new, {len(deviations) - len(new)} baselined)"
    )
    for key, line, kind, detail in new:
        print(f"  NEW  {args.spec}:{line} [{kind}] {key}\n       {detail}")
    for key, line in passes_in_baseline:
        print(f"  STALE baseline row {key} (spec line {line} now conforms) — remove it")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
