"""Compile exact tuple results and nullable payload boundaries."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "byte_projection": 'fn value(p: (i64, u8)) -> i64 { return p.1 as i64 }\nexport fn probe(v: i64) -> i64 { return value((v, 255 as u8)) }',
        "nullable_result": 'fn pair(s: string) -> (string, string)? { if (len(s) == 0) { return null }; return (s, "b") }\nexport fn probe(s: string) -> bool { return pair(s) != null }',
        "tuple_parameter": 'fn size(p: (string, string)) -> i64 { return len(p.0) + len(p.1) }\nexport fn probe(s: string) -> i64 { return size((s, s)) }',
        "owned_result": 'fn pair(s: string) -> (string, string) { return (s, s) }\nexport fn probe(s: string) -> i64 { var p = pair(s); return len(p.0) + len(p.1) }',
        "nullable_projection": 'fn pair(s: string) -> (string, string)? { if (len(s) == 0) { return null }; return (s, s) }\nexport fn probe(s: string) -> i64 { var p = pair(s); if (p == null) { return 0 }; return len(p.0) + len(p.1) }',
    }))
