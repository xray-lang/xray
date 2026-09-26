"""Compile managed spawn arguments through exact producer and callee authority."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {
        "suspending_constructor": 'import time\nfn worker() -> string { time.sleep(1); var out = StringBuilder(); out.append("ok"); return out.toString() }\nexport fn probe() -> i64 { go worker(); return 0 }',
        "suspending_helper": 'import time\nfn leaf() -> string { var out = StringBuilder(); out.append("ok"); return out.toString() }\nfn worker() -> string { time.sleep(1); return leaf() }\nfn start() { go worker() }\nexport fn probe() -> i64 { start(); return 0 }',
        "scalar": 'fn worker(n: i64) -> i64 { return n }\nexport fn probe(n: i64) -> i64 { go worker(n); return 0 }',
        "bool": 'fn worker(n: bool) -> bool { return n }\nexport fn probe(n: bool) -> i64 { go worker(n); return 0 }',
        "string": 'fn worker(s: string) -> i64 { return len(s) }\nexport fn probe(s: string) -> i64 { go worker(s); return 0 }',
        "move_array": 'fn worker(xs: move Array<i64>) -> i64 { return len(xs) }\nexport fn probe() -> i64 { var xs = [1,2]; go worker(move xs); return 0 }',
        "field": 'type Row = { name: string }\nfn worker(s: string) -> i64 { return len(s) }\nexport fn probe(s: string) -> i64 { var row: Row = { name: s }; go worker(row.name); return 0 }',
    }
    raise SystemExit(compile_cases(cases))
