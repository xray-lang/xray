// Rust reference for scalar_i64_loop.xr (statement-level isomorphic, safe std-only).
// Built by run_aot_c90_benchmarks.sh: rustc -C opt-level=3 -C lto=fat -C panic=abort

fn run(n: i64) -> i64 {
    let mut acc: i64 = 0;
    let mut i: i64 = 0;
    while i < n {
        acc += (i * 31) % 1000003;
        i += 1;
    }
    acc
}

fn main() {
    println!("{}", run(1000000));
}
