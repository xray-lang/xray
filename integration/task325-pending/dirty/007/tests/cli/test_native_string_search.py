"""Verify string search arities through source and strict host C compilation."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    cases = {
        "replace": 'export fn probe(s: string) -> i64 { return len(s.replace("a", "b")) }',
        "replace_all": 'export fn probe(s: string) -> i64 { return len(s.replaceAll("a", "b")) }',
        "replace_parameters": 'export fn probe(s: string, old: string, replacement: string) -> i64 { return len(s.replace(old, replacement)) }',
        "replace_chain": 'export fn probe(s: string) -> i64 { return len(s.replace("a", "b").replace("b", "c")) }',
        "last_index": "export fn probe(s: string, n: string) -> i64 { return s.lastIndexOf(n) }\n",
        "copy_bytes": "export fn probe(s: string) -> i64 { var bytes = s.copyBytes(); return len(bytes) }\n",
        "discard_bytes": "export fn probe(s: string) -> i64 { s.copyBytes(); return 42 }\n",
        "starts_with": "export fn probe(s: string, n: string) -> bool { return s.startsWith(n) }\n",
        "ends_with": "export fn probe(s: string, n: string) -> bool { return s.endsWith(n) }\n",
        "contains": "export fn probe(s: string, n: string) -> bool { return s.contains(n) }\n",
        "omitted": "export fn probe(s: string, n: string) -> i64 { return s.indexOf(n) }\n",
        "constant": "export fn probe(s: string, n: string) -> i64 { return s.indexOf(n, 1) }\n",
        "dynamic": "export fn probe(s: string, n: string, start: i64) -> i64 { return s.indexOf(n, start) }\n",
    }
    raise SystemExit(compile_cases(cases))
