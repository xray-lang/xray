"""Resource-tagged parallel scheduling.

Every shell suite used one `JOBS` for everything and `&` + `wait` to spend it.
That conflates work with different bottlenecks: generation and syntax-checking
are CPU-and-process bound and want every core, while linking is memory-and-disk
bound and thrashes if run at the same width. filetests even hard-coded its auto
cap at 4 for want of a way to say "these are different".

Here a task carries a resource tag, each tag has its own concurrency limit, and
a shared executor runs them subject to per-tag semaphores. Built on
concurrent.futures.ThreadPoolExecutor: the work is subprocess-bound, so threads
block on I/O and release the GIL -- the parallelism is real.
"""

from __future__ import annotations

import concurrent.futures
import threading
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional

from . import platform

# Resource classes and how wide each may run relative to the core count.
# Callers pick a tag; the mapping is the single place those widths are decided.
CPU = "cpu"        # generation, dump, syntax check: saturate cores
LINK = "link"      # native linking: memory + disk bound, throttle hard
RUN = "run"        # executing built binaries: ports/temp files, modest width
IO = "io"          # disk-heavy setup/copy: throttle


def default_limits(cores: "Optional[int]" = None) -> "Dict[str, int]":
    """Per-tag concurrency for a machine with `cores` logical CPUs."""
    n = cores if isinstance(cores, int) and cores >= 1 else platform.cpu_count()
    return {
        CPU: n,
        LINK: max(2, n // 4),
        RUN: max(2, n // 2),
        IO: max(2, n // 4),
    }


@dataclass
class Task:
    """One unit of work, tagged with the resource it contends for."""

    key: str
    fn: Callable
    tag: str = CPU


class Scheduler:
    """Run tagged tasks under per-tag concurrency caps.

    The executor is sized to the widest tag; per-tag semaphores keep each class
    within its own limit, so a burst of link tasks cannot starve or thrash even
    though CPU tasks are allowed to fill every core.
    """

    def __init__(self, limits: "Optional[Dict[str, int]]" = None) -> None:
        self._limits = limits or default_limits()
        self._sems = {tag: threading.Semaphore(n) for tag, n in self._limits.items()}
        self._max_workers = max(self._limits.values()) if self._limits else 1

    def _guarded(self, task: "Task") -> Callable:
        sem = self._sems.get(task.tag)

        def wrapped():
            if sem is None:
                return task.fn()
            with sem:
                return task.fn()

        return wrapped

    def run(
        self,
        tasks: "Iterable[Task]",
        on_done: "Optional[Callable]" = None,
    ) -> "Dict[str, object]":
        """Run all tasks, returning {key: result}. Order-independent.

        A task that raises stores the exception as its result rather than
        cancelling siblings: one bad case must not abort a whole suite. Callers
        inspect results and decide the verdict.

        on_done(key, result) fires once per completion, in the collecting
        thread, so a caller can drive a progress counter without its own lock.
        """
        task_list = list(tasks)
        if not task_list:
            return {}
        results: "Dict[str, object]" = {}
        workers = min(self._max_workers, len(task_list))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
            future_to_key = {
                executor.submit(self._guarded(task)): task.key for task in task_list
            }
            for future in concurrent.futures.as_completed(future_to_key):
                key = future_to_key[future]
                try:
                    results[key] = future.result()
                except Exception as exc:  # noqa: BLE001 - recorded, not swallowed
                    results[key] = exc
                if on_done is not None:
                    on_done(key, results[key])
        return results


def run_serial(
    tasks: "Iterable[Task]",
    on_done: "Optional[Callable]" = None,
) -> "Dict[str, object]":
    """Run tasks one at a time. For --jobs 1 and for deterministic debugging."""
    results: "Dict[str, object]" = {}
    for task in tasks:
        try:
            results[task.key] = task.fn()
        except Exception as exc:  # noqa: BLE001
            results[task.key] = exc
        if on_done is not None:
            on_done(task.key, results[task.key])
    return results
