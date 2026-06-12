// Rust reference for array_i64_sum.xr (statement-level isomorphic, safe std-only).
// Grows a Vec by push like the Xray source, then scans by index (bounds-checked).

fn run(n: i64) -> i64 {
    let mut values: Vec<i64> = Vec::new();
    let mut i: i64 = 0;
    while i < n {
        values.push((i * 17) % 251);
        i += 1;
    }

    let mut sum: i64 = 0;
    let mut j: usize = 0;
    while j < values.len() {
        sum += values[j];
        j += 1;
    }
    sum
}

fn main() {
    println!("{}", run(200000));
}
