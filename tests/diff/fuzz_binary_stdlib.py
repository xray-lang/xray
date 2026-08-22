#!/usr/bin/env python3
"""Seeded cross-oracle fuzzing for task-200 pure binary codecs.

The harness generates one type-correct Xray program from random byte strings,
then checks both VM and AOT output against Python's independent RFC 4648, hex,
and strict UTF-8 implementations. A mismatch keeps the generated source when
--keep-dir is supplied so the seed is directly reproducible.
"""

from __future__ import annotations

import argparse
import base64
import difflib
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


BOUNDARY_LENGTHS = (0, 1, 2, 3, 31, 32, 64, 255)


def xray_bool(value: bool) -> str:
    return "true" if value else "false"


def strict_utf8(data: bytes) -> bool:
    try:
        data.decode("utf-8", "strict")
        return True
    except UnicodeDecodeError:
        return False


def random_cases(seed: int, count: int) -> list[bytes]:
    rng = random.Random(seed)
    cases: list[bytes] = []
    for index in range(count):
        length = BOUNDARY_LENGTHS[index] if index < len(BOUNDARY_LENGTHS) else rng.randrange(513)
        cases.append(bytes(rng.randrange(256) for _ in range(length)))
    return cases


def generate_program(cases: list[bytes]) -> tuple[str, bytes]:
    lines = [
        "import base64",
        "import encoding",
        "import { Base64Alphabet, Base64DecodeOptions, Base64EncodeOptions, "
        "Base64PaddingPolicy } from base64",
        "",
        "fn urlEncodeOptions() -> Base64EncodeOptions {",
        "    var options = Base64EncodeOptions()",
        "    options.alphabet = Base64Alphabet.UrlSafe",
        "    options.padding = false",
        "    return options",
        "}",
        "",
        "fn urlDecodeOptions() -> Base64DecodeOptions {",
        "    var options = Base64DecodeOptions()",
        "    options.alphabet = Base64Alphabet.UrlSafe",
        "    options.padding = Base64PaddingPolicy.Forbidden",
        "    return options",
        "}",
        "",
    ]
    expected: list[str] = []

    for index, data in enumerate(cases):
        standard = base64.b64encode(data).decode("ascii")
        url = base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")
        hexed = data.hex()
        if data:
            values = ", ".join(str(value) for value in data)
            lines.append(f"var data{index}: Array<u8> = [{values}]")
        else:
            lines.append(f"var data{index} = Array<u8>(0)")
        lines.extend(
            [
                f"print(base64.encode(data{index}[:]))",
                f'var standardDecoded{index} = base64.decode("{standard}")',
                f"print(encoding.hexEncode(standardDecoded{index}[:]))",
                f"print(base64.encode(data{index}[:], urlEncodeOptions()))",
                f'var urlDecoded{index} = base64.decode("{url}", urlDecodeOptions())',
                f"print(encoding.hexEncode(urlDecoded{index}[:]))",
                f"print(encoding.hexEncode(data{index}[:]))",
                f"print(encoding.utf8Valid(data{index}[:]))",
                "",
            ]
        )
        expected.extend((standard, hexed, url, hexed, hexed, xray_bool(strict_utf8(data))))

    return "\n".join(lines) + "\n", ("\n".join(expected) + "\n").encode()


def generate_invalid_program() -> tuple[str, bytes]:
    lines = [
        "import base64",
        "import encoding",
        'print(base64.isValid("A"))',
        'print(base64.isValid("QR=="))',
        'print(base64.isValid("AA=A"))',
        'print(encoding.hexValid("0g"))',
        'print(encoding.hexValid("abc"))',
    ]
    return "\n".join(lines) + "\n", b"false\nfalse\nfalse\nfalse\nfalse\n"


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def report_mismatch(label: str, expected: bytes, actual: bytes) -> None:
    print(f"{label} output differs from independent oracle", file=sys.stderr)
    expected_lines = expected.decode("utf-8", "replace").splitlines(keepends=True)
    actual_lines = actual.decode("utf-8", "replace").splitlines(keepends=True)
    diff = difflib.unified_diff(expected_lines, actual_lines, fromfile="oracle", tofile=label)
    sys.stderr.writelines(list(diff)[:80])


def verify_backend(label: str, proc: subprocess.CompletedProcess[bytes], expected: bytes) -> bool:
    if proc.returncode != 0:
        print(f"{label} failed with exit code {proc.returncode}", file=sys.stderr)
        sys.stderr.buffer.write(proc.stderr[-8000:])
        return False
    if proc.stdout != expected:
        report_mismatch(label, expected, proc.stdout)
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xray", default=os.environ.get("XRAY_BIN", "build/xray"))
    parser.add_argument("--seed", type=int, default=200)
    parser.add_argument("--count", type=int, default=8)
    parser.add_argument("--chunk-size", type=int, default=8)
    parser.add_argument("--vm-only", action="store_true")
    parser.add_argument("--keep-dir")
    args = parser.parse_args()

    if args.count <= 0:
        parser.error("--count must be positive")
    if args.chunk_size <= 0 or args.chunk_size > 8:
        parser.error("--chunk-size must be between 1 and 8")
    root = Path(__file__).resolve().parents[2]
    xray = Path(args.xray)
    if not xray.is_absolute():
        xray = (root / xray).resolve()
    if not xray.is_file() or not os.access(xray, os.X_OK):
        print(f"xray executable not found: {xray}", file=sys.stderr)
        return 2

    cases = random_cases(args.seed, args.count)
    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_dir:
        work = Path(args.keep_dir).resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="xray-binary-fuzz-")
        work = Path(temporary.name)

    programs: list[tuple[str, str, bytes]] = []
    for chunk_index, start in enumerate(range(0, len(cases), args.chunk_size)):
        source, expected = generate_program(cases[start : start + args.chunk_size])
        programs.append((f"chunk_{chunk_index}", source, expected))
    invalid_source, invalid_expected = generate_invalid_program()
    programs.append(("invalid", invalid_source, invalid_expected))

    ok = True
    for label, source, expected in programs:
        source_path = work / f"binary_fuzz_seed_{args.seed}_{label}.xr"
        source_path.write_text(source, encoding="utf-8")
        (work / f"expected_{label}.txt").write_bytes(expected)

        vm = run([str(xray), "run", str(source_path)], root)
        if not verify_backend(f"VM {label}", vm, expected):
            ok = False
            break
        if args.vm_only:
            continue

        binary = work / f"binary_fuzz_aot_{label}"
        build = run([str(xray), "build", "--native", str(source_path), "-o", str(binary)], root)
        if build.returncode != 0:
            print(
                f"AOT build for {label} failed with exit code {build.returncode}",
                file=sys.stderr,
            )
            sys.stderr.buffer.write(build.stderr[-8000:])
            ok = False
            break
        if not verify_backend(f"AOT {label}", run([str(binary)], root), expected):
            ok = False
            break

    if ok:
        modes = "VM" if args.vm_only else "VM/AOT"
        print(f"OK: {args.count} seeded binary codec cases match Python and {modes} (seed={args.seed})")
    elif not args.keep_dir:
        keep = root / "build" / "binary-fuzz-failure"
        if keep.exists():
            shutil.rmtree(keep)
        shutil.copytree(work, keep)
        print(f"failing corpus saved to {keep}", file=sys.stderr)

    if temporary is not None:
        temporary.cleanup()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
