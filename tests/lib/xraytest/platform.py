"""Platform facts every other module builds on.

One place answers "what is different about this OS", so no suite grows its own
`uname -s` / `.exe` / CRLF branch. The shell tree had 53 such branches across
29 files; the point of the package is that this file is the only one.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

IS_WINDOWS = os.name == "nt"
IS_DARWIN = sys.platform == "darwin"

# Executable and static-library affixes. The AOT and toolchain code needs the
# real on-disk names, which differ only by platform, never by suite.
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""
STATIC_LIB_PREFIX = "" if IS_WINDOWS else "lib"
STATIC_LIB_SUFFIX = ".lib" if IS_WINDOWS else ".a"


def configure_utf8_stdio() -> None:
    """Make captured test reports byte-stable across host console locales.

    Windows inherits the active console code page for Python text streams.
    CTest captures bytes, so emitting UTF-8 directly is both portable and
    deterministic; it also preserves diagnostics containing non-ASCII source
    text instead of replacing or dropping them.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="strict")
    sys.stderr.reconfigure(encoding="utf-8", errors="strict")


def exe_name(stem: str) -> str:
    """Executable file name for a bare stem (`xray` -> `xray.exe` on Windows)."""
    return stem + EXE_SUFFIX if not stem.endswith(EXE_SUFFIX) else stem


def static_lib_name(stem: str) -> str:
    """Static library file name for a bare stem (`xray_core` -> `libxray_core.a`)."""
    return f"{STATIC_LIB_PREFIX}{stem}{STATIC_LIB_SUFFIX}"


def cpu_count() -> int:
    """Usable logical CPUs, never below 1.

    os.cpu_count() answers what getconf / sysctl did in shell, on every OS and
    without spawning a process. A None result (rare, but documented) floors to
    a serial run rather than crashing the scheduler.
    """
    count = os.cpu_count()
    return count if isinstance(count, int) and count >= 1 else 1


def env_int(name: str, default: int) -> int:
    """Read a positive integer environment override, falling back on garbage.

    Used for tunables like per-case timeouts and job caps, where a malformed
    value should quietly keep the default rather than crash a whole test run.
    """
    raw = os.environ.get(name)
    if raw is None:
        return default
    raw = raw.strip()
    if raw.isdigit() and int(raw) > 0:
        return int(raw)
    return default


def env_timeout(name: str, default: int) -> float | None:
    """Read a per-case timeout override with an explicit disable.

    Unset keeps `default`; "0" disables the timeout (returns None) so a lane can
    restore no-timeout behavior on purpose; a positive integer sets it; garbage
    keeps the default rather than surprising a run with a disabled timeout.
    """
    raw = os.environ.get(name)
    if raw is None:
        return float(default)
    raw = raw.strip()
    if raw.isdigit():
        value = int(raw)
        return float(value) if value > 0 else None
    return float(default)


def env_flag(name: str, default: bool = False) -> bool:
    """Read a boolean environment toggle with one shared spelling.

    Accepts the values shell tests already set (`1`, `true`, `yes`, `on`) so a
    migrated suite honors the same variables its shell form did.
    """
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def project_root() -> Path:
    """Repository root inferred from this file's location (tests/lib/xraytest)."""
    return Path(__file__).resolve().parents[3]
