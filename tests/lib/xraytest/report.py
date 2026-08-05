"""One result vocabulary and one way to print a suite summary.

Migrated suites report PASS / FAIL / SKIP through this, so a developer reading
CI output sees the same shape everywhere, and a wrapper can parse it without
learning each suite's ad-hoc wording. A SKIP is a real third state, never a
silent pass: the ratchet needs to know a case produced no verdict.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, TextIO


class Status(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass
class CaseResult:
    """Verdict for one case, with a reason that is required when not passing."""

    name: str
    status: Status
    detail: str = ""
    excerpt: str = ""

    def __post_init__(self) -> None:
        if self.status is not Status.PASS and not self.detail:
            # A red or skipped verdict with no reason is unactionable; forbid it
            # at construction so no suite can emit a bare FAIL.
            raise ValueError(f"{self.status.value} for {self.name!r} needs a detail")


@dataclass
class Report:
    """Accumulated case verdicts and the suite-level summary they imply."""

    suite: str
    cases: "List[CaseResult]" = field(default_factory=list)

    def add(self, result: CaseResult) -> None:
        self.cases.append(result)

    def record(self, name: str, status: Status, detail: str = "", excerpt: str = "") -> None:
        self.add(CaseResult(name=name, status=status, detail=detail, excerpt=excerpt))

    @property
    def passed(self) -> "List[CaseResult]":
        return [c for c in self.cases if c.status is Status.PASS]

    @property
    def failed(self) -> "List[CaseResult]":
        return [c for c in self.cases if c.status is Status.FAIL]

    @property
    def skipped(self) -> "List[CaseResult]":
        return [c for c in self.cases if c.status is Status.SKIP]

    @property
    def failed_names(self) -> "List[str]":
        return [c.name for c in self.failed]

    @property
    def skipped_names(self) -> "List[str]":
        return [c.name for c in self.skipped]

    def write(self, stream: "Optional[TextIO]" = None, *, verbose: bool = False) -> None:
        """Print per-case lines and a one-line summary.

        Failures always show their detail; verbose adds the captured excerpt so
        a developer can see the compiler or runtime output without rerunning.
        """
        out = stream or sys.stdout
        for case in self.cases:
            out.write(f"  {case.status.value:4}  {case.name}")
            if case.detail and case.status is not Status.PASS:
                out.write(f"  ({case.detail})")
            out.write("\n")
            if verbose and case.excerpt and case.status is Status.FAIL:
                for line in case.excerpt.splitlines():
                    out.write(f"        {line}\n")
        out.write(
            f"\n=== {self.suite}: {len(self.passed)} passed, "
            f"{len(self.failed)} failed, {len(self.skipped)} skipped ===\n"
        )

    @property
    def exit_code(self) -> int:
        """0 only when nothing failed. Skips do not fail the suite by themselves."""
        return 1 if self.failed else 0
