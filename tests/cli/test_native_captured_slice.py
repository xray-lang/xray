"""Compile a captured byte array used by a synchronous ref Slice callback."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {"captured_array": '''import mem
export fn probe() -> i64 {
    var source: Array<u8> = [7 as u8, 9 as u8]
    var out = mem.alloc(2)
    unsafe {
        mem.withSliceMut<u8>(out.borrowPtr(), 2, ref out,
            fn(destination: ref Slice<u8>) {
                var bytes: Slice<u8> = source[:]
                destination.copyFrom(bytes)
            })
    }
    return len(source)
}
'''}
    raise SystemExit(compile_cases(cases))
