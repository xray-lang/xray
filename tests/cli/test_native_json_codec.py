"""Compile typed JSON namespace codecs and their owned String boundaries."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "integer": 'export fn probe(n: i64) -> i64 { return len(JSON.stringify(n)) }',
        "boolean": 'export fn probe(n: bool) -> i64 { return len(JSON.stringify(n)) }',
        "float": 'export fn probe(n: f64) -> i64 { return len(JSON.stringify(n)) }',
        "string": 'export fn probe(n: string) -> i64 { return len(JSON.stringify(n)) }',
        "array": 'export fn probe(n: i64) -> i64 { return len(JSON.stringify([n, n])) }',
        "json_value": 'export fn probe(n: i64) -> i64 { return len(JSON.stringify(JSON.value(n))) }',
    }))
