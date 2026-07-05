// Rust reference for set_i64_membership.xr (safe, std HashSet<i64>).

use std::collections::HashSet;

fn run(n: i64, rounds: i64) -> i64 {
    var mut s: HashSet<i64> = HashSet::new();

    var mut i: i64 = 0;
    while i < n {
        s.insert(i * 2);
        i += 1;
    }

    var mut hits: i64 = 0;
    var mut r: i64 = 0;
    while r < rounds {
        i = 0;
        while i < n * 2 {
            if s.contains(&i) {
                hits += 1;
            }
            i += 1;
        }
        r += 1;
    }

    hits + s.len() as i64
}

fn main() {
    println!("{}", run(150000, 4));
}
