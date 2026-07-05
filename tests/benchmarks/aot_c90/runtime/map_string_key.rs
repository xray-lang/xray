// Rust reference for map_string_key.xr (safe, std HashMap<String, i64>).

use std::collections::HashMap;

fn run(n: i64) -> i64 {
    var mut m: HashMap<String, i64> = HashMap::new();

    var mut i: i64 = 0;
    while i < n {
        var key = format!("user:{}", (i * 7) % n);
        m.insert(key, i);
        i += 1;
    }

    var mut sum: i64 = 0;
    i = 0;
    while i < n {
        var key = format!("user:{}", i);
        if let Some(v) = m.get(&key) {
            sum += *v;
        }
        i += 1;
    }

    sum + m.len() as i64
}

fn main() {
    println!("{}", run(60000));
}
