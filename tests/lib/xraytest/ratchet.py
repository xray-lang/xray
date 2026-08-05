"""The only-shrink baseline ratchet, defined once.

Three files today (baseline_failures.txt, known_failures.txt,
filetests_known_failures.txt) encode the same policy in three shell
reimplementations. The policy:

  - a failure NOT in the baseline fails the run (a real regression);
  - a baseline entry that now PASSES fails the run (the line must be deleted,
    so a future regression cannot hide behind a stale allowance);
  - a case that was SKIPPED is neither: it produced no verdict, so it can be
    neither a new failure nor evidence that a baselined entry is fixed.

The list may only shrink. That is the whole contract, and it lives here.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from . import platform


def read_baseline(path: Path) -> "set[str]":
    """Parse a ratchet file: one case id per line, `#` comments, blanks ignored.

    Matches the shell `sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d'`.
    A missing file is an empty baseline, not an error -- a suite with nothing
    yet baselined still gates every case.
    """
    if not Path(path).is_file():
        return set()
    entries: "set[str]" = set()
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            entries.add(line)
    return entries


@dataclass(frozen=True)
class RatchetResult:
    """Verdict of one ratchet evaluation.

    ok is True only when nothing regressed and nothing baselined is now fixed.
    new_failures and now_passing are sorted for stable, diffable output.
    """

    new_failures: "list[str]"
    now_passing: "list[str]"
    failing_count: int
    baseline_count: int

    @property
    def ok(self) -> bool:
        return not self.new_failures and not self.now_passing


def evaluate(
    *,
    failed: Iterable,
    baseline: Iterable,
    skipped: Iterable | None = None,
) -> RatchetResult:
    """Apply the only-shrink policy to one run's verdicts.

    new_failures = failed - baseline
    now_passing  = (baseline - failed) - skipped

    Subtracting skipped from now_passing is the subtle part: a baselined case
    that did not run this time has not been shown to pass, so it must not be
    reported as a line to delete.
    """
    failed_set = set(failed)
    baseline_set = set(baseline)
    skipped_set = set(skipped or ())

    new_failures = sorted(failed_set - baseline_set)
    now_passing = sorted((baseline_set - failed_set) - skipped_set)

    return RatchetResult(
        new_failures=new_failures,
        now_passing=now_passing,
        failing_count=len(failed_set),
        baseline_count=len(baseline_set),
    )


def format_report(result: RatchetResult, *, baseline_path: str | None = None) -> str:
    """Human-facing summary mirroring the shell runners' wording."""
    where = f" ({baseline_path})" if baseline_path else ""
    lines: "list[str]" = []
    if result.new_failures:
        lines.append(f"=== New failures not in baseline{where} ===")
        lines.extend(f"  {name}" for name in result.new_failures)
        lines.append("Fix the case, or -- only with a written reason -- add it to the baseline.")
    if result.now_passing:
        lines.append("=== Baselined cases now pass; delete these entries ===")
        lines.extend(f"  {name}" for name in result.now_passing)
        lines.append("The baseline may only shrink. Remove the lines above.")
    lines.append(
        f"Ratchet: {result.failing_count} failing, {result.baseline_count} baselined."
    )
    return "\n".join(lines)
