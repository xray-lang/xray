"""Embed the exact ordinary test library in its frozen executable."""
from pathlib import Path
import argparse


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = args.input.read_bytes()
    if not data or len(data) >= 16384 or b"\0" in data:
        raise ValueError("Ordinary test library must be bounded UTF-8 text without NUL")
    data.decode("utf-8", errors="strict")
    rows = ["/* Immutable ordinary test library source, generated from its exact fixture. */",
            "static const char regex_library_source[] ="]
    for start in range(0, len(data), 64):
        rows.append('    "' + "".join("\\%03o" % byte for byte in data[start:start + 64]) + '"')
    rows[-1] += ";"
    encoded = ("\n".join(rows) + "\n").encode("ascii")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != encoded:
        args.output.write_bytes(encoded)


if __name__ == "__main__":
    main()
