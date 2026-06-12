// Rust reference for string_eq_hash.xr (safe, std HashMap<String, i64>).

use std::collections::HashMap;

fn run(n: i64) -> i64 {
    let mut keys: Vec<String> = Vec::new();
    let mut i: i64 = 0;
    while i < n {
        keys.push(format!("key-{}", (i * 31) % 1000));
        i += 1;
    }

    let mut counts: HashMap<String, i64> = HashMap::new();
    let mut j: usize = 0;
    while j < keys.len() {
        *counts.entry(keys[j].clone()).or_insert(0) += 1;
        j += 1;
    }

    let mut eq: i64 = 0;
    j = 0;
    while j + 1 < keys.len() {
        if keys[j] == keys[j + 1] {
            eq += 1;
        }
        j += 1;
    }

    counts.len() as i64 + eq + counts["key-0"]
}

fn main() {
    println!("{}", run(400000));
}
