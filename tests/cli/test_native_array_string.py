"""Verify fresh String ownership from Array members through strict C compilation."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {
        "class_field_push": 'export class Entries { values: Array<string>; constructor() { this.values = [] }; ref add(value: string) { this.values.push(value) } }; export fn probe(value: string) { var items = Entries(); items.add(value) }',
        "nullable_integer_store": 'export fn probe(h: i64?, m: i64?) -> Array<i64>? { if (h == null || m == null) { return null }; return [h!, m!, 0] }',
        "nullable_float_store": 'export fn probe(x: f64?) -> Array<f64>? { if (x == null) { return null }; return [x!, 1.5] }',
        "nullable_bool_store": 'export fn probe(x: bool?) -> Array<bool>? { if (x == null) { return null }; return [x!, false] }',
        "nullable_string_store": 'export fn probe(x: string?) -> Array<string>? { if (x == null) { return null }; return [x!, "tail"] }',
        "string_join": 'export fn probe() -> string { var xs = ["a", "b"]; return xs.join(",") }',
        "string_to_string": 'export fn probe() -> string { var xs = ["a", "b"]; return xs.toString() }',
        "integer_join": 'export fn probe() -> string { var xs = [1, 2]; return xs.join(",") }',
        "empty_join": 'export fn probe() -> string { var xs: Array<string> = []; return xs.join(",") }',
        "url_resolve": 'import url\nexport fn probe() -> string { return url.resolve("https://example.org/a/b", "../c") }',
        "two_results": 'export fn probe() -> i64 { var xs = ["a", "b"]; var a = xs.join(","); var b = xs.toString(); return len(a) + len(b) }',
    }
    raise SystemExit(compile_cases(cases, case_timeouts={"url_resolve": 120}))
