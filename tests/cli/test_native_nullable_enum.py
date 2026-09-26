"""Verify local and imported nullable unit-enum results in strict generated C."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    source = "export enum ProbeError { Bad, Worse }\nexport fn query(flag: bool) -> ProbeError? { if(flag) {return ProbeError.Bad}; return null }\n"
    cases = {
        "local": source + "export fn probe() -> bool { return query(true) != null }\n",
        "imported": 'import { query } from "./helper"\nexport fn probe() -> bool { return query(true) != null }\n',
        "imported_null": 'import { query } from "./helper"\nexport fn probe() -> bool { return query(false) == null }\n',
        "narrowed_compare": source + "export fn probe() -> bool {\n    var e = query(true)\n"
                            "    return e != null && e != ProbeError.Worse\n}\n",
        "nullable_member_compare": source +
            "export fn probe() -> bool { return query(true) == ProbeError.Bad }\n",
        "field_store": source + "class Holder {\n    kind: ProbeError\n"
                       "    constructor() { this.kind = ProbeError.Worse }\n}\n"
                       "export fn probe() -> bool { return Holder().kind == ProbeError.Worse }\n",
    }
    raise SystemExit(compile_cases(cases, extra_sources={"helper.xr": source}))
