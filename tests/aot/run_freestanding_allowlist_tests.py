#!/usr/bin/env python3
"""Freestanding stdlib allowlist: the list, the analyzer, and the probes agree.

Two independent things must hold. First, the module allowlist committed in
stdlib/freestanding_allowlist must match the set the analyzer actually enforces
-- these are two hand-maintained lists, and a drift means the gate below is
testing a different set than the compiler permits. Second, every listed module's
probe must compile to a no-libc relocatable object whose undefined symbols stay
inside the tiny hook/mem surface a freestanding image can provide.

Replaces run_freestanding_allowlist_tests.sh.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import List, Optional, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary as binlib  # noqa: E402
from xraytest import platform, proc, report, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

DEFAULT_ALLOWLIST = PROJECT_DIR / "stdlib" / "freestanding_allowlist"
ANALYZER_SOURCE = PROJECT_DIR / "src" / "frontend" / "analyzer" / "xanalyzer_visitor.c"

Status = report.Status

# The only undefined symbols a freestanding object may carry: the two host
# hooks it is allowed to import, plus the compiler-emitted mem intrinsics.
ALLOWED_UNDEFINED = frozenset({
    "xr_hook_panic", "xr_hook_write",
    "memcpy", "memmove", "memset", "memcmp",
})

# The analyzer enforces its allowlist with a chain of strcmp calls inside one
# function; this pulls the names back out of that function's body.
_ANALYZER_FUNC_RE = re.compile(
    r"XR_FUNC bool xa_freestanding_stdlib_module_allowed.*?\n\}", re.S
)
_STRCMP_RE = re.compile(r'strcmp\(module_name,\s*"([^"]*)"\)')

# A provider action proving no libc was linked. The COFF path emits one
# amalgamated translation unit instead of running a link stage, so it has no
# -nostdlib to show.
FREESTANDING_MARKERS = (
    "-nostdlib",
    "COFF relocatable: one amalgamated translation unit; no link stage",
)


def parse_allowlist(path: Path) -> Tuple[List[Tuple[str, str]], List[str]]:
    """(module, probe_path) pairs, plus malformed lines.

    Task 257 owns runner text normalization: strip the Windows record
    terminator before tokenizing a governed text file.
    """
    entries: List[Tuple[str, str]] = []
    bad: List[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\r").split("#", 1)[0]
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 2:
            bad.append(raw)
            continue
        entries.append((fields[0], fields[1]))
    return entries, bad


def analyzer_allowed_modules() -> List[str]:
    text = ANALYZER_SOURCE.read_text(encoding="utf-8", errors="replace")
    match = _ANALYZER_FUNC_RE.search(text)
    if not match:
        return []
    return sorted(set(_STRCMP_RE.findall(match.group(0))))


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Freestanding allowlist gate")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    allowlist = Path(os.environ.get("XRAY_FREESTANDING_ALLOWLIST", str(DEFAULT_ALLOWLIST)))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    if not allowlist.is_file():
        sys.stderr.write(f"FAIL: freestanding allowlist missing: {allowlist}\n")
        return 1

    entries, malformed = parse_allowlist(allowlist)
    passed = failed = 0

    # 1. The committed list and the analyzer's enforced set must agree.
    listed = sorted({module for module, _probe in entries})
    enforced = analyzer_allowed_modules()
    if listed == enforced:
        print("  PASS: analyzer module allowlist matches stdlib/freestanding_allowlist")
        passed += 1
    else:
        print("  FAIL: analyzer module allowlist drifted from stdlib/freestanding_allowlist")
        for name in sorted(set(listed) - set(enforced)):
            print(f"      only in allowlist: {name}")
        for name in sorted(set(enforced) - set(listed)):
            print(f"      only in analyzer:  {name}")
        failed += 1

    for raw in malformed:
        print(f"  FAIL: bad allowlist line: {raw}")
        failed += 1

    # 2. Every probe compiles freestanding and stays inside the symbol surface.
    with workspace.Workspace("xray_freestanding_allowlist") as ws:
        build_cache = ws.path(".cache")
        for module, probe_rel in entries:
            probe = PROJECT_DIR / probe_rel
            if not probe.is_file():
                print(f"  FAIL: {module}: probe source missing ({probe_rel})")
                failed += 1
                continue

            out = ws.path(f"{module}.o")
            result = proc.run(
                [xray, "build", "--native", "--profile", "freestanding", "--shared",
                 "--keep-c", "--rebuild", "--dump-link-command",
                 "--cache-dir", build_cache, "-o", out, probe],
                timeout=timeout,
            )
            log = result.combined_text()
            if not result.ok:
                print(f"  FAIL: {module}: probe build failed")
                for line in log.splitlines()[:120]:
                    print(f"      {line}")
                failed += 1
                continue

            if not any(marker in log for marker in FREESTANDING_MARKERS):
                print(f"  FAIL: {module}: probe lacks a freestanding relocatable provider action")
                for line in log.splitlines()[:80]:
                    print(f"      {line}")
                failed += 1
                continue

            if not out.is_file():
                print(f"  FAIL: {module}: expected object not produced")
                failed += 1
                continue

            undefined = binlib.undefined_symbol_names(out, timeout=timeout)
            if undefined is None:
                print(f"  FAIL: {module}: no symbol inspector available")
                failed += 1
                continue

            unexpected = [s for s in undefined if s not in ALLOWED_UNDEFINED]
            if unexpected:
                print(f"  FAIL: {module}: unexpected undefined symbols")
                for name in unexpected:
                    print(f"      {name}")
                failed += 1
                continue

            print(f"  PASS: {module}: freestanding probe")
            passed += 1

    print("")
    print(f"=== Results: {passed} passed, {failed} failed ===")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
