"""Matching compiler output against a `.expected` sibling file.

The rule every diagnostic gate shares: each non-empty line of the expected file
must appear somewhere in the output as a literal substring. Lines are literal,
not patterns -- a diagnostic that happens to contain regex metacharacters must
not silently match something else.

No ANSI stripping happens here, and none is needed: xray emits plain,
un-coloured diagnostics whenever stderr is not a terminal (xr_diag_use_color in
src/frontend/xdiag_fmt.h), and a runner always captures through a pipe.
"""

from pathlib import Path
from typing import List, Sequence


def expected_lines(path: Path) -> List[str]:
    """Non-empty lines of an expected-diagnostics file, in order.

    A missing file yields no requirements, matching the shell runners: the
    caller decides whether that is an error for its gate.
    """
    if not path.is_file():
        return []
    return [line for line in path.read_text(encoding="utf-8").splitlines() if line]


def missing_lines(output: str, expected: Sequence[str]) -> List[str]:
    """Expected lines absent from output, in the order they were declared."""
    return [line for line in expected if line not in output]
