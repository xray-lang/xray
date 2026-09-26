"""Verify owned String copy through strict generated C compilation."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "literal": 'export fn probe() -> i64 { var s = copy("hello"); return len(s) }\n',
        "owned": 'fn duplicate(s: string) -> string { return copy(s) }\nexport fn probe() -> i64 { var s = "a" + "bc"; var t = duplicate(s); return len(s) + len(t) }\n',
        "field": 'export fn probe() -> i64 { var a = { name: "hello" }; var s = copy(a.name); return len(s) }\n',
        "empty": 'export fn probe() -> i64 { return len(copy("")) }\n',
    }))
