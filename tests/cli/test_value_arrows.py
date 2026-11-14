"""Exercise arrow roles through public syntax, checking, VM and native commands."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

VALUE_ERROR = "use `=>` for lambdas, match arms and select arms; `->` only introduces a type"
TYPE_ERROR = "use `->` before a return type or in a function type; `=>` maps a value"


def command(arguments: list[str], expected: int = 0) -> subprocess.CompletedProcess:
    result = subprocess.run(arguments, capture_output=True, timeout=30)
    if result.returncode != expected:
        raise AssertionError(f"{arguments!r}: {result.returncode}\n{result.stdout!r}\n{result.stderr!r}")
    return result


def negative(binary: str, directory: Path, name: str, source: str, error: str, arrow: str) -> None:
    path = directory / (name + ".xr")
    path.write_bytes(source.encode("utf-8"))
    result = command([binary, "check", str(path)], 1)
    text = result.stderr.decode("utf-8")
    first = next((line for line in text.splitlines() if "error:" in line), "")
    assert error in first, (name, first)
    at = source.index(arrow)
    line = source[:at].count("\n") + 1
    column = at - source.rfind("\n", 0, at)
    assert re.search(rf":{line}:{column}(?::|\b)", first), (name, line, column, text)
    replacement = "replace `->` with `=>`" if arrow == "->" else "replace `=>` with `->`"
    assert replacement in text, (name, text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = str(args.binary.resolve(strict=True))
    fixtures = Path(__file__).resolve().parents[1] / "fixtures" / "value_arrows"
    command([binary, "check", str(fixtures / "check.xr")])
    command([binary, "check", "-S", str(fixtures / "syntax.xr")])
    cases = {
        "bare": "const bad = x -> x + 1\n",
        "empty": "const bad = () -> 1\n",
        "typed": "const bad = (x: i64) -> x + 1\n",
        "block": "const bad = (x: i64) -> { return x }\n",
        "match": "const bad = match (0) { 0 -> 1, _ => 2, }\n",
        "guard": "const bad = match (0) { n if (n == 0) -> 1, _ => 2, }\n",
        "receive": "fn bad() { select { value from ch -> {} } }\n",
        "send": "fn bad() { select { 1 to ch -> {} } }\n",
        "timeout": "fn bad() { select { after 10 -> {} } }\n",
        "default": "fn bad() { select { _ -> {} } }\n",
        "interpolation": 'const bad = "prefix ${((x: i64) -> x)(1)}"\n',
        "raw_interpolation": 'const bad = r"prefix ${((x: i64) -> x)(1)}"\n',
        "block_interpolation": 'const bad = """\r\n    prefix ${((x: i64) -> x)(1)}\r\n    """\n',
        "nested_interpolation": 'const bad = "${r"inside ${((x: i64) -> x)(1)}"}"\n',
    }
    types = {
        "return": "fn bad() => i64 { return 1 }\n",
        "function_type": "const bad: fn(i64) => i64 = x => x\n",
        "fn_lambda": "const bad = fn(x: i64) => i64 { return x }\n",
        "nosuspend": "fn bad(action: nosuspend fn(i64) => i64) {}\n",
        "escaping": "fn bad(action: escaping fn(i64) => i64) {}\n",
        "method": "struct Bad { value() => i64 { return 1 } }\n",
        "interpolation_type": 'const bad = "${fn(x: i64) => i64 { return x }}"\n',
    }
    with tempfile.TemporaryDirectory(prefix="value-arrows.") as temporary:
        directory = Path(temporary)
        for name, source in cases.items():
            negative(binary, directory, name, source, VALUE_ERROR, "->")
        for name, source in types.items():
            negative(binary, directory, name, source, TYPE_ERROR, "=>")
        for name, source in {
            "literal_block": "const bad = (x: i64) => {value: x}\n",
            "missing_comma": "const bad = match (0) { 0 => 1\n _ => 2 }\n",
        }.items():
            path = directory / (name + ".xr")
            path.write_text(source, encoding="utf-8")
            result = command([binary, "check", "-S", str(path)], 1)
            if name == "missing_comma":
                assert b"expected ',' between match arms" in result.stderr, result.stderr
        source = fixtures / "runtime.xr"
        expected = b"8\n7\n9\n12\n11\n"
        assert command([binary, "run", str(source)]).stdout == expected
        product = directory / ("arrows.exe" if os.name == "nt" else "arrows")
        command([binary, "build", str(source), "-o", str(product)])
        assert command([str(product)]).stdout == expected
    print(f"value arrows: positive syntax/check, {len(cases) + len(types) + 2} rejections, VM/native golden output passed")


if __name__ == "__main__":
    main()
