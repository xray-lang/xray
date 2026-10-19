"""Compile Atomic compare-exchange result carriers with strict host C checks."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {}
    for kind, value, desired in (("i64", "42", "43"), ("f64", "1.5", "2.5"),
                                  ("bool", "false", "true")):
        for ordering in ("", ", Ordering.AcquireRelease"):
            name = kind + ("_ordered" if ordering else "_default")
            cases[name] = (f"export fn probe() -> bool {{ var cell = Atomic<{kind}>({value}); "
                           f"var (_, ok) = cell.compareExchange({value}, {desired}{ordering}); "
                           "return ok }")
        cases[kind + "_load_store_swap"] = (
            f"export fn probe() -> bool {{ var cell = Atomic<{kind}>({value}); "
            f"cell.store({desired}, Ordering.Release); "
            f"var previous = cell.swap({value}, Ordering.AcquireRelease); "
            f"return previous == {desired} && cell.load(Ordering.Acquire) == {value} }}")
    cases["class_field_retry"] = """
class Counter {
    private cell: Atomic<i64>
    constructor() { this.cell = Atomic<i64>(42) }
    decrement() -> bool {
        var current = this.cell.load(Ordering.Acquire)
        var (_, ok) = this.cell.compareExchange(current, current - 1, Ordering.AcquireRelease)
        return ok
    }
}
export fn probe() -> bool { var counter = Counter(); return counter.decrement() }
"""
    cases["class_field_loop"] = cases["class_field_retry"].replace(
        "var current =", "while (true) { var current =").replace(
        "return ok", "if (ok) { return true }; }; return false")
    helper = cases["class_field_loop"].split("export fn probe")[0].replace("class Counter", "export class Counter")
    cases["imported_class_field_loop"] = 'import { Counter } from "./atomic_helper"\nexport fn probe() -> bool { var counter = Counter(); return counter.decrement() }'
    raise SystemExit(compile_cases(cases, extra_sources={"atomic_helper.xr": helper}))
