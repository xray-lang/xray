#!/usr/bin/env python3
"""Verify the real SHA library against hashlib and isolated diagnostic probes."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

MAX_BYTES = 1048576
MAX_CASES = 8192
MAX_CHUNKS = 8192


def cases():
    vectors = []
    alignments = (0, 1, 3, 7)
    boundaries = (0, 1, 55, 56, 63, 64, 65, 127, 128, 129)
    for length in tuple(range(130)) + (2048, 4096, 65535, 65536):
        data = bytes((index * 37 + length * 11) & 255 for index in range(length))
        for alignment in alignments:
            vectors.append((data, alignment, ()))
            if length <= 129:
                vectors.append((data, alignment, (0,) + (1,) * length + (0,)))
            if length in boundaries:
                for split in range(length + 1):
                    vectors.append((data, alignment, (0, split, 0, length - split, 0)))
        if length > 129:
            remaining, chunks = length, [0]
            while remaining:
                amount = min(remaining, (1, 3, 55, 64, 127, 1024)[len(chunks) % 6])
                chunks.extend((amount, 0))
                remaining -= amount
            vectors.append((data, 7, tuple(chunks)))
    # Published empty/abc/56-byte vectors and the million-a vector complement
    # binary-pattern inputs. Expected results always come from independent hashlib.
    for data in (b"", b"abc", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", b"a" * 1000000):
        vectors.append((data, 1, ()))
    assert 0 < len(vectors) <= MAX_CASES
    for data, alignment, chunks in vectors:
        assert len(data) <= MAX_BYTES and 0 <= alignment < 8
        assert len(chunks) <= MAX_CHUNKS and (not chunks or sum(chunks) == len(data))
    return vectors


def execute(executable, argument, env=None):
    return subprocess.run([str(executable), str(argument)], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60, env=env)


def verify_vectors(executable):
    vectors = cases()
    with tempfile.TemporaryDirectory(prefix="xray-sha-identity-") as directory:
        packet = Path(directory) / "vectors.bin"
        with packet.open("wb") as file:
            file.write(b"XRSHAT01" + struct.pack("<I", len(vectors)))
            for data, alignment, chunks in vectors:
                file.write(struct.pack("<III", len(data), alignment, len(chunks)))
                for amount in chunks:
                    file.write(struct.pack("<I", amount))
                file.write(data)
        result = execute(executable, packet)
    assert result.returncode == 0, result.stderr.decode("utf-8", errors="replace")
    # C text-mode stdout is CRLF on Windows and LF on Unix. Digests themselves
    # remain strict ASCII; binary packets are opened in rb/wb on both sides.
    lines = result.stdout.decode("ascii").splitlines()
    assert len(lines) == len(vectors), (len(lines), len(vectors))
    for index, ((data, _, _), line) in enumerate(zip(vectors, lines)):
        expected = f"{index} {hashlib.sha256(data).hexdigest()}"
        assert line == expected, (index, len(data), line, expected)
    print(f"SHA256 independent oracle: {len(vectors)} bounded binary/chunk/alignment cases passed")


def verify_probe(executable, argument, diagnostic, option=None):
    env = os.environ.copy()
    if option:
        key, setting = option
        inherited = env.get(key, "")
        env[key] = (inherited + ":" if inherited else "") + setting
    result = execute(executable, argument, env)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    assert result.returncode != 0 and re.search(diagnostic, output), (
        argument, result.returncode, output)
    print(f"SHA256 real library probe {argument}: expected diagnostic observed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    for flag in ("debug", "asan", "ubsan"):
        parser.add_argument("--" + flag, required=True, type=int, choices=(0, 1))
    args = parser.parse_args()
    verify_vectors(args.executable)
    if args.debug:
        verify_probe(args.executable, "--debug-null", r"\[FATAL\].*xr_sha256_init: NULL ctx")
    if args.asan:
        verify_probe(args.executable, "--asan-overread", r"AddressSanitizer: global-buffer-overflow",
                     ("ASAN_OPTIONS", "halt_on_error=1:abort_on_error=1"))
    if args.ubsan:
        verify_probe(args.executable, "--ubsan-misaligned", r"runtime error:.*misaligned address",
                     ("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1"))


if __name__ == "__main__":
    main()
