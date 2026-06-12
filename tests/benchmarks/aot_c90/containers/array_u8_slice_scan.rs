// Rust reference for array_u8_slice_scan.xr (statement-level isomorphic, safe std-only).
// u8 accumulator wraps each round, matching Xray's sub-width wrap semantics.

fn run(n: i64) -> i64 {
    let mut bytes: Vec<u8> = Vec::new();
    let mut i: i64 = 0;
    while i < n {
        bytes.push(i as u8);
        i += 1;
    }

    let mid = &bytes[17..(n - 17) as usize];
    let mut sum: u8 = 0;
    let mut j: usize = 0;
    while j < mid.len() {
        sum = sum.wrapping_add(mid[j]);
        j += 1;
    }
    sum as i64
}

fn main() {
    println!("{}", run(200000));
}
