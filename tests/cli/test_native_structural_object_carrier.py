"""Verify structural field access preserves exact Array and merge carriers."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {
        "nullable_local": 'type Row = { name: string }\nexport fn probe() -> string { var row: Row? = { name: "a" }; return row!.name }',
        "nullable_module": 'type Row = { name: string }\nvar row: Row? = null\nexport fn probe() -> string { row = { name: "a" }; return row!.name }',
        "index": '''type Row = { name: string }
fn read(xs: Array<Row>, index: i64) -> string { return xs[index].name }
export fn probe(index: i64) -> string {
    var xs: Array<Row> = [{ name: "a" }]
    return read(xs, index)
}''',
        "merge": '''type Row = { name: string }
fn read(flag: bool, a: Row, b: Row) -> string { var row = flag ? a : b; return row.name }
export fn probe(flag: bool) -> string {
    var a: Row = { name: "a" }
    var b: Row = { name: "b" }
    return read(flag, a, b)
}''',
        "nested_index": '''type Row = { child: { name: string } }
fn read(xs: Array<Row>, index: i64) -> string { return xs[index].child.name }
export fn probe(index: i64) -> string {
    var xs: Array<Row> = [{ child: { name: "a" } }]
    return read(xs, index)
}''',
    }
    cases["nullable_call_merge"] = 'type Row = { name: string }\nfn make() -> Row? { return { name: "a" } }\nexport fn probe(flag: bool) -> string { var row = flag ? make() : null; if (row == null) { return "empty" }; return row!.name }\n'
    for scalar in ("u64", "i64", "f64", "bool"):
        cases["scalar_call_" + scalar] = (
            "type Row = { value: " + scalar + " }\n"
            "fn take(value: " + scalar + ") -> " + scalar + " { return value }\n"
            "export fn probe(value: " + scalar + ") -> " + scalar +
            " { var row: Row = { value: value }; return take(row.value) }\n"
        )
    raise SystemExit(compile_cases(cases))
