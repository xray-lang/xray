// Rust reference for sort_i64_random.xr (safe, std sort_unstable = pdqsort).

fn run(n: i64) -> i64 {
    var mut values: Vec<i64> = Vec::new();
    var mut seed: i64 = 123456789;
    var mut i: i64 = 0;
    while i < n {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        values.push(seed);
        i += 1;
    }

    values.sort_unstable();

    var nu = n as usize;
    var mut checksum: i64 = values[0] + values[nu / 2] + values[nu - 1];
    var mut j: usize = 0;
    while j < nu {
        checksum += values[j] % 7;
        j += 7919;
    }
    checksum
}

fn main() {
    println!("{}", run(1000000));
}
