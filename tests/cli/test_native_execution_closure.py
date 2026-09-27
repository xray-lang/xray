"""Compile live exports without materializing unrelated dependency bodies."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "module_local_operation_indexes": 'import { left } from "./left"\nimport { right } from "./right"\nexport fn probe(text: string) -> string { return left(text) + right(text) }',
        "cold_io_dependency": 'import { XmlDiagnostic } from xml\nexport fn probe(value: XmlDiagnostic) -> string { return value.message }',
        "live_path_method": 'import { Path } from path\nexport fn probe(text: string) -> string { var value = Path(text); return value.toString() }',
        "cold_class_payload_projection": '''import "./cold_payload"
class Counter {
    constructor() {}
}
export fn probe() -> i64 {
    var counter = Counter()
    return 7
}''',
    }, {
        "left.xr": 'export fn left(text: string) -> string { return text.replace("a", "b") }',
        "right.xr": 'export fn right(text: string) -> string { return text.replace("c", "d") }',
        "cold_payload.xr": '''export class Payload {
    value: i64
    constructor(value: i64) { this.value = value }
}
export enum Envelope {
    Some { payload: Payload },
    None
}
export fn cold(node: Envelope) -> i64 {
    return match (node) {
        Envelope.Some { payload } -> payload.value,
        _ -> 0
    }
}''',
    }, case_timeouts={"cold_io_dependency": 180}))
