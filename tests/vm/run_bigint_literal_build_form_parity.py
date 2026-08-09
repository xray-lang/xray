#!/usr/bin/env python3
"""BigInt literals survive the default (embedded-bytecode) `xray build`.

Regression for the constant-pool round-trip. The default `xray build` embeds a
serialized bytecode image, and BigInt literal constants were serialized as NULL
because the value serializer had no BigInt case at all -- the writer fell
through to its "other types not supported" default. A built binary therefore
printed `null` for `123n` and evaluated `123n > 100n` as `false`, while
`xray run` (source VM) produced `123`/`true`. The fix teaches the constant
(de)serializer a BigInt tag so the literal survives the embed/reload cycle.

The same source is driven through all three execution forms:

  1. `xray run <src>`            -- source VM; the semantic oracle.
  2. `xray build <src>`          -- default embedded-bytecode binary (the fix).
  3. `xray build --native <src>` -- AOT native backend.

The embedded-bytecode binary must reproduce the oracle byte for byte: that is
the round-trip this test guards, and it is a hard gate. The bytecode artifact is
also compiled twice and reloaded via `xray run <artifact>` -- a toolchain-free
path that exercises the same serializer -- to confirm the BigInt constant
serializes deterministically and reloads to the oracle output.

All three forms are gated across the full BigInt surface: read-only (compare /
toString / print), arithmetic (+ - * / %, unary minus), bitwise (& | ^), and
shift (<< >>). The AOT native backend reproduces the oracle byte for byte for
every one of them, so this test also guards AOT BigInt from regressing.

Usage: run_bigint_literal_build_form_parity.py <xray>
"""

from __future__ import annotations

import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

# BigInt literal coverage: every literal base, values on both sides of the
# int64 boundary, sign, comparisons, and arithmetic whose result is a fresh
# BigInt. Every line prints a stable decimal or boolean -- no addresses.
SOURCE = b"""\
print(0n)
print(123n)
print(-456n)
print(0xFFn)
print(0xFFFFFFFFFFFFFFFFn)
print(0b1111n)
print(0o777n)
print(99999999999999999999999999999999n)
print(12345678901234567890n)
var b = 123n
print(b > 100n)
print(b < 100n)
print(b == 123n)
print((b + 1000n).toString())
print((100000000000000000000n + 1n).toString())
print((5n - 8n).toString())
print((-100n / 7n).toString())
print((-100n % 7n).toString())
print((99999999999n * 99999999999n).toString())
print((2n - 5n) < 0n)
print((-123456789012345678901234567890n).toString())
print((12n & 10n).toString())
print((-12n | 10n).toString())
print((12n ^ 10n).toString())
print((3n << 4).toString())
print((1n << 100).toString())
print((123456789012345678901234567890n >> 33).toString())
"""

EXPECTED = (
    b"0\n"
    b"123\n"
    b"-456\n"
    b"255\n"
    b"18446744073709551615\n"
    b"15\n"
    b"511\n"
    b"99999999999999999999999999999999\n"
    b"12345678901234567890\n"
    b"true\n"
    b"false\n"
    b"true\n"
    b"1123\n"
    b"100000000000000000001\n"
    b"-3\n"
    b"-14\n"
    b"-2\n"
    b"9999999999800000000001\n"
    b"true\n"
    b"-123456789012345678901234567890\n"
    b"8\n"
    b"-2\n"
    b"6\n"
    b"48\n"
    b"1267650600228229401496703205376\n"
    b"14372261824592212087\n"
)


def _fail(msg: str, result: proc.ProcResult | None = None) -> int:
    sys.stderr.write(f"FAIL: {msg}\n")
    if result is not None:
        sys.stderr.write(result.combined_text())
        if not result.stdout.endswith(b"\n"):
            sys.stderr.write("\n")
    return 1


def _mismatch(msg: str, got: bytes) -> int:
    sys.stderr.write(f"FAIL: {msg}\n")
    sys.stderr.write(f"  expected: {EXPECTED!r}\n")
    sys.stderr.write(f"  got:      {got!r}\n")
    return 1


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        sys.stderr.write(f"usage: {argv[0]} <xray>\n")
        return 2

    xray = Path(argv[1])
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    with workspace.Workspace("xray-bigint-build-form") as ws:
        src = ws.write("bigint_forms.xr", SOURCE)

        # Form 1: source VM -- the oracle. Pin it so a VM-side regression does
        # not silently make the other forms "match" a wrong baseline.
        oracle = proc.run([xray, "run", src], timeout=timeout)
        if not oracle.ok:
            return _fail("source VM run failed", oracle)
        if oracle.stdout != EXPECTED:
            return _mismatch("source VM output is not the expected oracle", oracle.stdout)

        # Bytecode artifact: deterministic serialization, and reload parity via
        # a toolchain-free `xray run <artifact>`. This is the purest exercise of
        # the constant-pool (de)serializer the fix touches.
        art_a, art_b = ws.path("forms-a.xrc"), ws.path("forms-b.xrc")
        for art in (art_a, art_b):
            compiled = proc.run([xray, "compile", "-f", "bytecode", "-o", art, src],
                                timeout=timeout)
            if not compiled.ok:
                return _fail("bytecode compilation failed", compiled)
        if art_a.read_bytes() != art_b.read_bytes():
            return _fail("bytecode is not deterministic: two compilations differ")
        reload = proc.run([xray, "run", art_a], timeout=timeout)
        if not reload.ok:
            return _fail("running the bytecode artifact failed", reload)
        if reload.stdout != EXPECTED:
            return _mismatch("bytecode reload dropped or degraded a BigInt literal",
                             reload.stdout)

        # Form 2: default embedded-bytecode build -- the form the fix targets.
        binary = ws.path("bigint_forms_embed")
        built = proc.run([xray, "build", src, "-o", binary], timeout=timeout)
        if not built.ok:
            return _fail("default (embedded-bytecode) build failed", built)
        embed = proc.run([binary], timeout=timeout)
        if not embed.ok:
            return _fail("running the embedded-bytecode binary failed", embed)
        if embed.stdout != EXPECTED:
            return _mismatch("embedded-bytecode binary dropped or degraded a BigInt literal",
                             embed.stdout)

        # Form 3: AOT native backend. Read-only BigInt (compare / toString /
        # print) and arithmetic (+ - * / %, unary minus) now reproduce the
        # oracle byte for byte, so this is a hard gate. Bitwise (& | ^) and
        # shift (<< >>) are deliberately not exercised here: they still lower
        # through the int64 path and are tracked as a separate remaining gap.
        binary_n = ws.path("bigint_forms_native")
        built_n = proc.run([xray, "build", "--native", src, "-o", binary_n], timeout=timeout)
        if not built_n.ok:
            return _fail("AOT native build failed", built_n)
        native = proc.run([binary_n], timeout=timeout)
        if not native.ok:
            return _fail("running the AOT native binary failed", native)
        if native.stdout != EXPECTED:
            return _mismatch("AOT native BigInt output diverges from the oracle", native.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
