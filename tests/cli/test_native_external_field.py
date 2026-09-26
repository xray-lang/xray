"""Verify imported fields and call boundaries through strict C compilation."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    source = """import { Item, read } from "./item"
final class Holder {
 item: Item
 constructor(item: Item) { this.item = item }
}
export fn probe() -> i64 {
 const holder = Holder(Item())
 return read(holder.item)
}
"""
    dependency = """export final class Item {
 value: i64
 constructor() { this.value = 42 }
}
export fn read(item: const Item) -> i64 { return item.value }
"""
    cases = {"const_field": source, "mutable_field": source.replace("const holder", "var holder")}
    cases["default_slice_options"] = """export class Options {
 enabled: bool = true
}
export fn probe(data: Slice<u8>, options: Options = Options()) -> i64 {
 if (options.enabled) { return len(data) }
 return 0
}
"""
    cases["declared_slice_string"] = 'export fn probe(data: Slice<u8>) -> string { if (len(data) == 0) { return "empty" }; return "data" }'
    cases["declared_slice_nullable_string"] = 'export fn probe(data: Slice<u8>) -> string? { if (len(data) == 0) { return null }; return "data" }'
    cases["imported_string_argument"] = """import text
import { make } from "./record"
export fn probe() -> i64 { var p = make(); return len(text.lower(p.protocol)) }
"""
    record = """export class Record {
 protocol: string
 constructor(value: string) { this.protocol = value }
}
export fn make() -> Record { return Record("HTTP:") }
"""
    cases["imported_nullable_options_slice"] = 'import { Record } from "./record"\nexport fn probe(data: Slice<u8>, options: Record?) -> i64 { return len(data) }'
    cases["imported_getter"] = 'import { make } from "./record"\nexport fn probe() -> i64 { var p = make(); return p.size }'
    cases["imported_method_argument"] = 'import { make } from "./record"\nexport fn probe() -> i64 { var p = make(); return p.plus(37) }'
    record = record.replace(' constructor(value:', ' size: i64 { fn() { return len(this.protocol) } }\n plus(n: i64) -> i64 { return len(this.protocol) + n }\n constructor(value:')
    optional = """export fn integer(s: string) -> i64? { if (len(s) == 0) { return null }; return 42 }
export fn flag(s: string) -> bool? { if (len(s) == 0) { return null }; return true }
export fn number(s: string) -> f64? { if (len(s) == 0) { return null }; return 1.5 }
export fn byte(s: string) -> u8? { if (len(s) == 0) { return null }; return 42 as u8 }
"""
    optional += 'export fn text(s: string) -> string? { if (len(s) == 0) { return null }; return s + "!" }\n'
    cases["nullable_string_namespace"] = ('import "./optional" as values\n'
        'export fn probe(s: string) -> bool { return values.text(s) != null }')
    for function in ("integer", "flag", "number", "byte", "text"):
        cases["nullable_" + function] = (f'import {{ {function} }} from "./optional"\n'
            f'export fn probe(s: string) -> bool {{ return {function}(s) != null }}')
    record = record.replace(' plus(n:', ' maybe(n: i64) -> i64? { if (n == 0) { return null }; return n }\n plus(n:')
    cases["nullable_method"] = ('import { make } from "./record"\n'
        'export fn probe() -> bool { var p = make(); return p.maybe(1) != null }')
    record = record.replace(' plus(n:', ' extend(s: string) -> Record { return Record(this.protocol + s) }\n text() -> string { return this.protocol + "!" }\n plus(n:')
    cases["owned_class_method"] = ('import { make } from "./record"\n'
        'export fn probe() -> i64 { var p = make(); var q = p.extend("!"); return len(q.protocol) }')
    cases["owned_class_method_constructor"] = ('import { Record } from "./record"\n'
        'export fn probe() -> i64 { var p = Record("HTTP:"); var q = p.extend("!"); return len(q.protocol) }')
    cases["owned_string_method"] = ('import { make } from "./record"\n'
        'export fn probe() -> i64 { var p = make(); return len(p.text()) }')
    raise SystemExit(compile_cases(cases, {"item.xr": dependency, "record.xr": record,
                                         "optional.xr": optional}))
