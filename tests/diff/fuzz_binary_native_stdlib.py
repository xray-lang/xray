#!/usr/bin/env python3
"""Seeded cross-oracle fuzzing for binary stdlib algorithms.

Compress is source-only Xray; crypto still owns private provider leaves.  This
harness groups them by byte-oriented behavior, not by implementation strategy,
and compares both against Python's zlib, hashlib, hmac, and shape checks under
the VM and native AOT backends.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import hmac
import os
import random
import shutil
import string
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


BOUNDARY_LENGTHS = (0, 1, 2, 3, 31, 32, 64, 127)
TEXT_ALPHABET = string.ascii_letters + string.digits + " .,:;_-+/=!?@#%&*()[]{}"
HMAC_ALGORITHMS = ("md5", "sha1", "sha256", "sha512")


def xray_bool(value: bool) -> str:
    return "true" if value else "false"


def xray_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def random_text_cases(seed: int, count: int) -> list[str]:
    rng = random.Random(seed)
    cases: list[str] = []
    for index in range(count):
        length = BOUNDARY_LENGTHS[index] if index < len(BOUNDARY_LENGTHS) else rng.randrange(129)
        cases.append("".join(rng.choice(TEXT_ALPHABET) for _ in range(length)))
    return cases


def digest_lines(data: bytes, key: bytes) -> list[str]:
    return [
        hashlib.md5(data).hexdigest(),
        hashlib.sha1(data).hexdigest(),
        hashlib.sha256(data).hexdigest(),
        hashlib.sha512(data).hexdigest(),
        *(hmac.new(key, data, getattr(hashlib, algo)).hexdigest() for algo in HMAC_ALGORITHMS),
    ]


def generate_program(cases: list[str], seed: int) -> tuple[str, bytes]:
    lines = [
        "import compress",
        "import crypto",
        "",
    ]
    expected: list[str] = []

    for index, text in enumerate(cases):
        key = f"key-{seed}-{index}-{len(text)}"
        data = text.encode("utf-8")
        key_bytes = key.encode("utf-8")
        lines.extend(
            [
                f"var text{index} = {xray_string(text)}",
                f"var key{index} = {xray_string(key)}",
                f"print(compress.crc32(text{index}.bytes()))",
                f"print(compress.adler32(text{index}.bytes()))",
                f"print(crypto.md5(text{index}))",
                f"print(crypto.sha1(text{index}))",
                f"print(crypto.sha256(text{index}))",
                f"print(crypto.sha512(text{index}))",
                f'print(crypto.hmac("md5", key{index}, text{index})!)',
                f'print(crypto.hmac("sha1", key{index}, text{index})!)',
                f'print(crypto.hmac("sha256", key{index}, text{index})!)',
                f'print(crypto.hmac("sha512", key{index}, text{index})!)',
                # Compressed payloads are Array<u8>; a view of one has to be
                # taken from a bound owner, never from a call's result directly.
                f"var gz{index} = compress.gzip(text{index}.bytes())",
                f"print(compress.isGzip(gz{index}[:]))",
                f"var ungz{index} = compress.gunzip(gz{index}[:])",
                f"print(string.fromUtf8(ungz{index}[:])! == text{index})",
                f"var deflated{index} = compress.deflate(text{index}.bytes())",
                f"var inflated{index} = compress.inflate(deflated{index}[:])",
                f"print(string.fromUtf8(inflated{index}[:])! == text{index})",
                f"var zlibbed{index} = compress.zlibCompress(text{index}.bytes())",
                f"print(compress.isZlib(zlibbed{index}[:]))",
                f"var unzlib{index} = compress.zlibDecompress(zlibbed{index}[:])",
                f"print(string.fromUtf8(unzlib{index}[:])! == text{index})",
                f'print(crypto.hmac("sha224", key{index}, text{index}) == null)',
                "",
            ]
        )
        expected.extend(
            [
                str(zlib.crc32(data)),
                str(zlib.adler32(data)),
                *digest_lines(data, key_bytes),
                "true",
                "true",
                "true",
                "true",
                "true",
                "true",
            ]
        )

    return "\n".join(lines) + "\n", ("\n".join(expected) + "\n").encode()


def generate_shape_program() -> tuple[str, bytes]:
    lines = [
        "import crypto",
        "",
        "fn isHex(text: string) -> bool {",
        "    var bytes: Slice<u8> = text.bytes()",
        "    for (var i = 0; i < len(bytes); i++) {",
        "        var c: i64 = bytes[i]",
        "        var digit = c >= 48 && c <= 57",
        "        var lower = c >= 97 && c <= 102",
        "        if (!digit && !lower) { return false }",
        "    }",
        "    return true",
        "}",
        "",
        'print(crypto.timingSafeEqual("abc", "abc"))',
        'print(crypto.timingSafeEqual("abc", "abd"))',
        'print(crypto.timingSafeEqual("abc", "ab"))',
        'print(crypto.timingSafeEqual("", ""))',
        "var r8 = crypto.randomBytes(8)",
        "var r16 = crypto.randomBytes(16)",
        "print(len(r8) == 16)",
        "print(isHex(r8))",
        "print(len(r16) == 32)",
        "print(isHex(r16))",
        "var uuid = crypto.uuid()",
        "var parts = uuid.split(\"-\")",
        "var variant = uuid.runes().nth(19)",
        "print(len(uuid) == 36)",
        "print(len(parts) == 5)",
        "print(len(parts[0]) == 8)",
        "print(len(parts[1]) == 4)",
        "print(len(parts[2]) == 4)",
        "print(len(parts[3]) == 4)",
        "print(len(parts[4]) == 12)",
        "print(uuid.runes().nth(14) == '4')",
        "print(variant == '8' || variant == '9' || variant == 'a' || variant == 'b')",
    ]
    expected = "\n".join(
        [
            "true",
            "false",
            "false",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
            "true",
        ]
    )
    return "\n".join(lines) + "\n", (expected + "\n").encode()


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
    parser.add_argument("--seed", type=int, default=2003)
    parser.add_argument("--count", type=int, default=8)
    parser.add_argument("--chunk-size", type=int, default=4)
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

    texts = random_text_cases(args.seed, args.count)
    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_dir:
        work = Path(args.keep_dir).resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="xray-binary-native-fuzz-")
        work = Path(temporary.name)

    programs: list[tuple[str, str, bytes]] = []
    for chunk_index, start in enumerate(range(0, len(texts), args.chunk_size)):
        source, expected = generate_program(texts[start : start + args.chunk_size], args.seed)
        programs.append((f"chunk_{chunk_index}", source, expected))
    shape_source, shape_expected = generate_shape_program()
    programs.append(("shape", shape_source, shape_expected))

    ok = True
    for label, source, expected in programs:
        source_path = work / f"binary_native_fuzz_seed_{args.seed}_{label}.xr"
        source_path.write_text(source, encoding="utf-8")
        (work / f"expected_{label}.txt").write_bytes(expected)

        vm = run([str(xray), "run", str(source_path)], root)
        if not verify_backend(f"VM {label}", vm, expected):
            ok = False
            break
        if args.vm_only:
            continue

        binary = work / f"binary_native_fuzz_aot_{label}"
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
        print(
            f"OK: {args.count} seeded binary stdlib algorithm cases match Python and "
            f"{modes} (seed={args.seed})"
        )
    elif not args.keep_dir:
        keep = root / "build" / "binary-native-fuzz-failure"
        if keep.exists():
            shutil.rmtree(keep)
        shutil.copytree(work, keep)
        print(f"failing corpus saved to {keep}", file=sys.stderr)

    if temporary is not None:
        temporary.cleanup()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
