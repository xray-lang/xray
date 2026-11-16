"""Exercise finite data union through the real CLI; no runtime qualification."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

CASES = [
    ("path_array", 'var values: Array<string | i64> = [1, "hello"]\n', True, ""),
    ("reordered", 'var values: Array<i64 | string> = [1, "hello"]\n', True, ""),
    ("duplicate", 'var values: Array<string | i64 | string> = [1, "hello"]\n', True, ""),
    ("duplicate_scalar", 'var value: i32 | i32 = 7\n', True, ""),
    ("empty", 'var values: Array<i64 | string> = []\n', True, ""),
    ("alias", 'type Value = i64 | string\nvar value: Value = "hello"\n', True, ""),
    ("parameter", 'fn use(value: i64 | string) {}\nuse(value: 7)\n', True, ""),
    ("return", 'fn value() -> i64 | string { return 7 }\nvar got = value()\n', True, ""),
    ("integer_context", 'var value: i32 | string = 7\n', True, ""),
    ("negative_context", 'var value: i8 | string = -128\n', True, ""),
    ("float_context", 'var value: f32 | string = 1.5\n', True, ""),
    ("integer_float_context", 'var value: f64 | string = 7\n', True, ""),
    ("integer_widen", 'var small: i8 = 7\nvar value: i32 | string = small\n', True, ""),
    ("array_store", 'var values: Array<i64 | string> = [1, "hello"]\nvalues[0] = "changed"\n', True, ""),
    ("wrong_member", 'var value: i64 | string = true\n', False, "unique admissible data-union member"),
    ("same_integer_family", 'var value: i16 | i32 = 7\n', False, "E0390"),
    ("same_float_family", 'var value: f32 | f64 = 1.5\n', False, "E0390"),
    ("integer_range", 'var value: i8 | string = 128\n', False, "outside its contextual type"),
    ("float_exact", 'var value: f64 | string = 9007199254740993\n', False, "not exactly representable"),
    ("typed_no_float", 'var integer: i64 = 7\nvar value: f64 | string = integer\n', False, "unique admissible data-union member"),
    ("signedness", 'var small: u8 = 7\nvar value: i32 | string = small\n', False, "unique admissible data-union member"),
    ("error_member", 'var value: Error | i64 = 7\n', False, "E0390"),
    ("nominal_member", 'struct Value { number: i64 }\nvar value: Value | i64 = 7\n', False, "E0390"),
    ("implicit_heterogeneous", 'var values = [1, "hello"]\n', False, "E0326"),
    ("error_union_unchanged", 'enum Left { Fault }\nenum Right { Fault }\nfn fail() throws(Left | Right) { throw Left.Fault }\n', True, ""),
]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    env = os.environ.copy()
    env["XRAY_STDLIB_PATH"] = str(args.root / "stdlib")
    with tempfile.TemporaryDirectory(prefix="xray-data-union-") as temporary:
        folder = Path(temporary)
        for name, source, accepted, message in CASES:
            path = folder / f"{name}.xr"
            path.write_text(source, encoding="utf-8")
            checked = subprocess.run([str(args.binary), "check", str(path)], cwd=args.root,
                                     env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
            assert (checked.returncode == 0) == accepted, (name, checked.returncode, checked.stdout, checked.stderr)
            assert not message or message in checked.stderr, (name, message, checked.stderr)
        # A successful static check is not an executable union representation.
        path = folder / "path_array.xr"
        for command in [["run", str(path)], ["build", str(path), "-o", str(folder / "union.exe")]]:
            refused = subprocess.run([str(args.binary), *command], cwd=args.root, env=env,
                                     capture_output=True, text=True, encoding="utf-8", timeout=30)
            assert refused.returncode != 0, (command, "unexpected execution qualification")
    print(f"finite data union CLI: {len(CASES)} semantic cases; run/build refused")

if __name__ == "__main__":
    main()
