// Rust reference for string_concat_builder.xr (safe; mirrors the Xray
// rebuild-per-concat pattern: each `s + t` produces a fresh String).

fn run(rounds: i64, parts: i64) -> i64 {
    var mut total: i64 = 0;
    var mut r: i64 = 0;
    while r < rounds {
        var mut s = String::new();
        var mut i: i64 = 0;
        while i < parts {
            s = s + "part-" + &(i % 10).to_string() + ";";
            i += 1;
        }
        total += s.len() as i64;
        r += 1;
    }
    total
}

fn main() {
    println!("{}", run(4000, 40));
}
