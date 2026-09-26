"""Verify Array reads after imported ref calls through strict C compilation."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    helper = "export fn change(dst: ref Array<u8>) { dst[0] = 9 as u8 }"
    cases = {
        "imported_ref_then_read": 'import { change } from "./ref_helper"\nexport fn probe() -> i64 { var data: Array<u8> = [1 as u8]; change(ref data); return data[0] as i64 }',
        "imported_ref_then_call": 'import { change } from "./ref_helper"\nfn read(data: Array<u8>) -> i64 { return data[0] as i64 }\nexport fn probe() -> i64 { var data: Array<u8> = [1 as u8]; change(ref data); return read(data) }',
    }
    raise SystemExit(compile_cases(cases, extra_sources={"ref_helper.xr": helper}))
