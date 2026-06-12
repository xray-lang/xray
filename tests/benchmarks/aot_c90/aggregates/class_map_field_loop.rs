// Rust reference for class_map_field_loop.xr (statement-level isomorphic, safe std-only).
// Uses std HashMap as the Rust-quality baseline; keeps the source's has+get
// double-lookup access pattern instead of if-let single lookup.

use std::collections::HashMap;

struct IntMapBag {
    values: HashMap<i64, i64>,
}

impl IntMapBag {
    fn new() -> Self {
        IntMapBag {
            values: HashMap::new(),
        }
    }

    fn fill(&mut self, n: i64) -> i64 {
        let mut i: i64 = 0;
        while i < n {
            self.values.insert(i, i * 3 + 1);
            i += 1;
        }
        self.values.len() as i64
    }

    fn scan(&self, n: i64, rounds: i64) -> i64 {
        let mut r: i64 = 0;
        let mut sum: i64 = 0;
        while r < rounds {
            let mut i: i64 = 0;
            while i < n {
                if self.values.contains_key(&i) {
                    sum += self.values[&i];
                }
                i += 1;
            }
            r += 1;
        }
        sum
    }
}

fn run(n: i64, rounds: i64) -> i64 {
    let mut bag = IntMapBag::new();
    let count = bag.fill(n);
    bag.scan(n, rounds) + count
}

fn main() {
    println!("{}", run(64, 5000));
}
