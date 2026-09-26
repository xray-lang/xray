"""Compile tuple elements through exact array ownership and strict C."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "indexed_field": 'export fn probe(s: string) -> i64 { var a: Array<(string, string)> = []; a.push((s, s)); return len(a[0].0) }',
        "strings": 'export fn probe() -> i64 { var a: Array<(string, string)> = []; a.push(("a", "b")); return len(a) }',
        "nested": 'export fn probe() -> i64 { var a: Array<((string, string), i64)> = []; a.push((("a", "b"), 42)); return len(a) }',
        "repeated_owner": 'export fn probe() -> i64 { var b = StringBuilder(); b.append("abc"); var s = b.toString(); var a: Array<(string, string)> = []; a.push((s, s)); b.clear(); return len(a) }',
    }))
