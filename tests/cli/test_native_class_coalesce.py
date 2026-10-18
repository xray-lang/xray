"""Compile nullable class coalescing with distinct branch flow facts."""
from source_product_compile import compile_cases

if __name__ == "__main__":
    declaration = "final class Options { value: i64; init() { this.value = 42 } }\n"
    cases = {
        "parameter": declaration + "fn read(options: Options?) -> i64 { var selected = options ?? Options(); return selected.value }\nexport fn probe() -> i64 { return read(null) }",
        "present": declaration + "fn read(options: Options?) -> i64 { var selected = options ?? Options(); return selected.value }\nexport fn probe() -> i64 { return read(Options()) }",
    }
    raise SystemExit(compile_cases(cases))
