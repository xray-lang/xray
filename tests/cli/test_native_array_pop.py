"""Verify nullable Array.pop results through strict generated C compilation."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    cases = {
        "string": 'export fn probe() -> i64 { var a: Array<string> = ["abc"]; var v = a.pop(); if (v == null) { return 0 }; return len(v) }\n',
        "empty": 'export fn probe() -> bool { var a: Array<string> = []; return a.pop() == null }\n',
        "discard": 'export fn probe() -> i64 { var a: Array<string> = ["abc"]; a.pop(); return len(a) }\n',
        "integer": 'export fn probe() -> i64 { var a: Array<i64> = [42]; var v = a.pop(); if (v == null) { return 0 }; return v }\n',
        "bool": 'export fn probe() -> bool { var a: Array<bool> = [true]; var v = a.pop(); if (v == null) { return false }; return v }\n',
        "float": 'export fn probe() -> f64 { var a: Array<f64> = [2.5]; var v = a.pop(); if (v == null) { return 0.0 }; return v }\n',
    }
    for value in (0, 255):
        cases[f"u8_{value}"] = f"export fn probe() -> i64 {{ var a: Array<u8> = [{value} as u8]; var v = a.pop(); if (v == null) {{ return -1 }}; return v as i64 }}\n"
    cases["u8_empty_used"] = "export fn probe() -> i64 { var a: Array<u8> = []; var v = a.pop(); if (v == null) { return -1 }; return v as i64 }\n"
    cases["u8_empty"] = "export fn probe() -> bool { var a: Array<u8> = []; return a.pop() == null }\n"
    cases["u8_discard"] = "export fn probe() -> i64 { var a: Array<u8> = [42 as u8]; a.pop(); return len(a) }\n"
    cases["nested_array"] = "export fn probe() -> i64 { var a: Array<Array<u8>> = [[7 as u8, 8 as u8]]; var v = a.pop(); if (v == null) { return -1 }; return len(v) }\n"
    for name, source in tuple(cases.items()):
        cases["shift_" + name] = source.replace(".pop()", ".shift()")
    raise SystemExit(compile_cases(cases))
