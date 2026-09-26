"""Compile a declared consuming receiver through exact ownership storage."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    source = """class Writer {
 value: i64
 constructor() { this.value = 42 }
 move finish() -> i64 { return this.value }
}
export fn probe() -> i64 {
 var writer = Writer()
 return (move writer).finish()
}
"""
    raise SystemExit(compile_cases({"owned_receiver": source}))
