"""Check brace collections, argument labels and formatter stability through the CLI."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def command(arguments: list[str], expected: int = 0) -> subprocess.CompletedProcess:
    result = subprocess.run(arguments, capture_output=True, timeout=30, env={**os.environ, "NO_COLOR": "1"})
    assert result.returncode == expected, (arguments, result.returncode, result.stdout, result.stderr)
    return result


def set_construction(binary: str, root: Path) -> None:
    positives = [
        'const value=Set([1,2,1])\n',
        'const value=Set<i32>([1,2,1])\n',
        'const value=Set<i64>()\n',
        'const value:Set<string>=Set()\n',
        'const value:Set<string>=Set([])\n',
        'fn array()->Array<i64>{return [1,2]}\nconst value=Set(array())\n',
        'fn main(){const array=[1,2];const value=Set(array);print(len(value))}\nmain()\n',
        'const value=Set([... [1,2],3])\n',
        'fn empty(value:Set<i64>=Set())->i64{return len(value)}\nprint(empty())\n',
        'fn Set(value:i64)->i64{return value}\nprint(Set(1))\n',
        'const value=[Set<i32>([1]),Set([2])]\n',
    ]
    negatives = [
        ('const value=Set()\n', 'empty Set construction requires an element type or Set context'),
        ('const value=Set(1)\n', 'Set construction requires an Array value'),
        ('const value=Set([1], [2])\n', 'Set construction takes one ordinary Array argument'),
        ('const value=Set([1,"a"])\n', 'E0326: Array literal elements have different types: i64 and string'),
        ('const value=Set<i64>(["a"])\n', 'expression cannot satisfy its declared type'),
    ]
    for index, text in enumerate(positives):
        path = root / f'set-positive-{index}.xr'
        path.write_text(text, encoding='utf-8')
        command([binary, 'check', str(path)])
        command([binary, 'fmt', str(path)])
        command([binary, 'check', str(path)])
        command([binary, 'fmt', '--check', str(path)])
    for index, (text, message) in enumerate(negatives):
        path = root / f'set-negative-{index}.xr'
        path.write_text(text, encoding='utf-8')
        result = command([binary, 'check', str(path)], 1)
        first = next(line for line in result.stderr.decode('utf-8').splitlines() if 'error:' in line)
        assert message in first, (index, first)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    binary = str(parser.parse_args().binary.resolve(strict=True))
    source = '''struct Point {
    x: i64
    y: i64
}
fn request(url: string, method: string = "GET", redirects: bool = true) -> string {
    return url
}
const first = request("a", redirects: false)
const second = request(redirects: false, url: "b")
const refs = swap(left: ref a, right: move b)
const map = {...defaults, makeKey(): makeValue(), "extra": 3, ...overrides}
const empty: Map<string, i64> = {}
const set = Set([1, 2, 3])
const point = Point{...base, x: value(), ...other}
const method = owner.update(right: 1, left: 2)
const constructed = Array<i64>(capacity: 8)
const lambda = x => ({"answer": x})
const selected = match (flag) { true => ({"yes": 1}), _ => ({"no": 0}) }
const template = "${request(url: "${key(name: "x")}")}"
'''
    rejected = [
        ('const value = #{"x": 1}\n', "Map prefix syntax was removed; use {...}"),
        ('const value = #[1, 2]\n', "Set literal syntax was removed; use Set([...])"),
        ('const value = ["x": 1]\n', "Map literals use braces: {key: value}"),
        ('type Shape = { x: i64 }\n', "structural object types were removed; declare a struct or interface"),
        ('fn inspect<T: { x: i64 }>(v: T) {}\n', "structural object types were removed; declare a struct or interface"),
        ('var {x} = value\n', "object destructuring was removed; use named field access"),
        ('const value = match (input) { {x} => x, _ => 0 }\n',
         "object patterns were removed; match a tuple or use named field access"),
        ('const value = {x}\n', "expected ':' after Map key"),
    ]
    with tempfile.TemporaryDirectory(prefix="collection-syntax-") as directory:
        root = Path(directory)
        set_construction(binary, root)
        for wrap in (False, True):
            path = root / ("wrapped.xr" if wrap else "plain.xr")
            path.write_bytes(source.encode("utf-8"))
            command([binary, "check", "-S", str(path)])
            options = ["--wrap", "-L", "48"] if wrap else []
            command([binary, "fmt", *options, str(path)])
            first = path.read_bytes()
            command([binary, "check", "-S", str(path)])
            command([binary, "fmt", *options, str(path)])
            assert path.read_bytes() == first, (wrap, first, path.read_bytes())
            command([binary, "fmt", "--check", *options, str(path)])
            text = first.decode("utf-8")
            for preserved in ("left: ref a", "right: move b", "...defaults", "...overrides", "...base", "...other"):
                assert preserved in text, (preserved, text)
            assert "#{" not in text and "#[" not in text
            assert "=> ({" in text, text
        for index, (text, message) in enumerate(rejected):
            path = root / f"rejected-{index}.xr"
            path.write_bytes(text.encode("utf-8"))
            result = command([binary, "check", "-S", str(path)], 1)
            first_error = next(line for line in result.stderr.decode("utf-8").splitlines() if "error:" in line)
            assert message in first_error, (index, first_error, result.stderr)
    print("Brace collections and labels: syntax, retired-form rejection and formatter round trips PASS")


if __name__ == "__main__":
    main()
