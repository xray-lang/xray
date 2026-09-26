"""Compile caller-proved raw Slice construction as portable C11."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {}
    for element in ("u8", "i64", "f64", "bool", "rune"):
        cases["raw_view_" + element] = f"""import mem
export fn probe(source: Array<{element}>) -> i64 {{
 var window: Slice<{element}> = source[:]
 unsafe {{ var raw = mem.slice<{element}>(window.ptr(), len(window), window); return len(raw) }}
}}
"""
    cases["reinterpret_empty"] = """import mem
export fn probe(source: Array<u8>) -> i64 {
 var window: Slice<u8> = source[:]
 unsafe { var raw = mem.slice<u32>(window.ptr(), 0, window); return len(raw) }
}
"""
    raise SystemExit(compile_cases(cases))
