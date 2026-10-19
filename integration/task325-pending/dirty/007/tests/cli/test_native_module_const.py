"""Compile module const publication and mutable/local controls through frozen plans."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {
        "module_const": 'const xs: Array<i64> = [1, 2]\nexport fn probe() -> i64 { return xs[0] }',
        "module_var": 'var xs: Array<i64> = [1, 2]\nexport fn probe() -> i64 { return xs[0] }',
        "local_const": 'export fn probe() -> i64 { const xs: Array<i64> = [1, 2]; return xs[0] }',
    }
    raise SystemExit(compile_cases(cases))
