"""Verify ref Slice descriptor calls through strict generated C compilation."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    fill = 'fn fill(dst: ref Slice<u8>) { dst[0] = 9 as u8 }\n'
    cases = {
        "direct": fill + 'export fn probe() -> i64 { var a: Array<u8> = [1 as u8]; var s: Slice<u8> = a[:]; fill(ref s); return a[0] as i64 }\n',
        "forwarded": fill + 'fn forward(dst: ref Slice<u8>) { fill(ref dst) }\nexport fn probe() -> i64 { var a: Array<u8> = [1 as u8]; var s: Slice<u8> = a[:]; forward(ref s); return a[0] as i64 }\n',
        "copy_from": 'fn fill(dst: ref Slice<u8>, src: Slice<u8>) { dst.copyFrom(src) }\nexport fn probe() -> i64 { var a: Array<u8> = [1 as u8]; var b: Array<u8> = [9 as u8]; var dst: Slice<u8> = a[:]; var src: Slice<u8> = b[:]; fill(ref dst, src); return a[0] as i64 }\n',
    }
    raise SystemExit(compile_cases(cases))
