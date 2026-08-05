"""A parallel-safe progress line for interactive runs, silent under CI.

The one rule that makes this safe to leave always-on: it writes to stderr only
when stderr is a tty. ctest, CI, and any redirected run capture stderr, so
`isatty()` is false there and nothing is emitted -- a `\r`-updated line would
otherwise shred a captured log. Locally the developer sees a live counter.

It counts completions, not submissions, so the number is meaningful under the
scheduler's out-of-order parallelism.
"""

from __future__ import annotations

import sys
import threading
from typing import TextIO

from . import platform


class ProgressReporter:
    def __init__(
        self,
        total: int,
        *,
        stream: "TextIO | None" = None,
        enabled: "bool | None" = None,
    ) -> None:
        self._total = max(0, total)
        self._done = 0
        self._lock = threading.Lock()
        self._stream = stream or sys.stderr
        if enabled is None:
            # tty and not explicitly muted. XRAY_TEST_NO_PROGRESS forces off for
            # a developer who wants clean output without redirecting.
            enabled = self._stream.isatty() and not platform.env_flag("XRAY_TEST_NO_PROGRESS")
        self._enabled = enabled
        self._width = 0

    def tick(self, label: str = "") -> None:
        """Record one completion and refresh the line. Safe from many threads."""
        with self._lock:
            self._done += 1
            if not self._enabled:
                return
            text = f"[{self._done}/{self._total}] {label}"
            # Pad to erase the previous, possibly longer, line.
            pad = max(0, self._width - len(text))
            self._stream.write("\r" + text + " " * pad)
            self._stream.flush()
            self._width = len(text)

    def finish(self) -> None:
        """Clear the progress line so it does not linger before the summary."""
        with self._lock:
            if self._enabled and self._width:
                self._stream.write("\r" + " " * self._width + "\r")
                self._stream.flush()
            self._width = 0
