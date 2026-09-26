"""Compile live exports without materializing unrelated dependency bodies."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "module_local_operation_indexes": 'import { left } from "./left"\nimport { right } from "./right"\nexport fn probe(text: string) -> string { return left(text) + right(text) }',
        "cold_io_dependency": 'import { XmlDiagnostic } from xml\nexport fn probe(value: XmlDiagnostic) -> string { return value.message }',
        "live_path_method": 'import { Path } from path\nexport fn probe(text: string) -> string { var value = Path(text); return value.toString() }',
    }, {
        "left.xr": 'export fn left(text: string) -> string { return text.replace("a", "b") }',
        "right.xr": 'export fn right(text: string) -> string { return text.replace("c", "d") }',
    }, case_timeouts={"cold_io_dependency": 180}))
