"""Compile executable string concatenation recipes as strict portable C."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    cases = {
        "string_parts": """export fn probe(value: string) -> string {
 return "left" + value + "right"
}
""",
        "scalar_parts": """export fn probe(value: i64) -> string {
 return "value=" + string(value) + ";"
}
""",
    }
    raise SystemExit(compile_cases(cases))
