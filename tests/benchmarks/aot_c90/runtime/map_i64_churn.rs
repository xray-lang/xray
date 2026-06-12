// Rust reference for map_i64_churn.xr (safe, std HashMap with the default hasher).

use std::collections::HashMap;

fn run(n: i64) -> i64 {
    let mut m: HashMap<i64, i64> = HashMap::new();

    let mut i: i64 = 0;
    while i < n {
        m.insert((i * 17) % n, i * 3 + 1);
        i += 1;
    }

    let mut sum: i64 = 0;
    i = 0;
    while i < n * 2 {
        if let Some(v) = m.get(&i) {
            sum += *v;
        }
        i += 1;
    }

    i = 0;
    while i < n {
        if i % 2 == 0 {
            m.remove(&i);
        }
        i += 1;
    }

    i = 0;
    while i < n {
        if m.contains_key(&i) {
            sum += 1;
        }
        i += 1;
    }

    sum + m.len() as i64
}

fn main() {
    println!("{}", run(200000));
}
