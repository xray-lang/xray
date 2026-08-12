#!/usr/bin/env python3
"""BigInt literals survive every execution form, byte for byte.

The embedded-bytecode `xray build` once wrote BigInt constants to the constant
pool as null, so the built binary printed null where `xray run` printed the
value. The native `xray build -N` backend never touches that serializer, so it
kept the value.

This runner takes the source run as the oracle and requires the other two
forms to reproduce its stdout exactly:

  1. `xray build`    then run the binary -- embedded bytecode;
  2. `xray build -N` then run the binary -- native AOT.

The fixture prints only BigInt values (no comparisons, no int64-overflowing
arithmetic) so that every backend is expected to agree; a divergence here is a
serialization or codegen regression, not a fixture that leans on a backend quirk.

Usage: run_bigint_bytecode_roundtrip.py <xray> <fixture.xr>
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402


def _require_ok(result: proc.ProcResult, what: str) -> int | None:
    if result.ok:
        return None
    sys.stderr.write(f"{what} failed (exit {result.returncode}"
                     f"{', timed out' if result.timed_out else ''})\n")
    sys.stderr.write(result.combined_text())
    return 1


def _require_match(oracle: bytes, actual: bytes, form: str) -> int | None:
    if oracle == actual:
        return None
    sys.stderr.write(f"{form} disagrees with the source run\n")
    sys.stderr.write(f"  source: {oracle!r}\n")
    sys.stderr.write(f"  {form}: {actual!r}\n")
    return 1


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        sys.stderr.write(f"usage: {argv[0]} <xray> <fixture.xr>\n")
        return 2

    xray, fixture = Path(argv[1]), Path(argv[2])
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    with workspace.Workspace("xray-bigint-bytecode") as ws:
        entry = ws.path("entry.xr")
        shutil.copy2(fixture, entry)

        source = proc.run([xray, "run", entry], timeout=timeout)
        if (rc := _require_ok(source, "running the source")) is not None:
            return rc
        oracle = source.stdout

        # 1. Embedded-bytecode build (the form that regressed).
        bc_bin = ws.path(platform.exe_name("entry_bytecode"))
        built = proc.run([xray, "build", "-o", bc_bin, entry], timeout=timeout)
        if (rc := _require_ok(built, "building the embedded-bytecode binary")) is not None:
            return rc
        bc_run = proc.run([bc_bin], timeout=timeout)
        if (rc := _require_ok(bc_run, "running the embedded-bytecode binary")) is not None:
            return rc
        if (rc := _require_match(oracle, bc_run.stdout, "embedded-bytecode build")) is not None:
            return rc

        # 2. Native AOT build.
        native_bin = ws.path(platform.exe_name("entry_native"))
        native_built = proc.run([xray, "build", "-N", "-o", native_bin, entry],
                                timeout=timeout)
        if (rc := _require_ok(native_built, "building the native binary")) is not None:
            return rc
        native_run = proc.run([native_bin], timeout=timeout)
        if (rc := _require_ok(native_run, "running the native binary")) is not None:
            return rc
        if (rc := _require_match(oracle, native_run.stdout, "native build")) is not None:
            return rc

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
