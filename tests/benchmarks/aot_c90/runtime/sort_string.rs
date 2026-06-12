// Rust reference for sort_string.xr (safe, std Vec<String> sort_unstable;
// String orders by byte content like the Xray runtime comparison).

fn run(n: i64) -> i64 {
    let mut values: Vec<String> = Vec::new();
    let mut i: i64 = 0;
    while i < n {
        values.push(format!("item-{}-tail", (i * 37) % 5000));
        i += 1;
    }

    values.sort_unstable();

    let nu = n as usize;
    let mut checksum: i64 =
        values[0].len() as i64 + values[nu / 2].len() as i64 + values[nu - 1].len() as i64;
    let mut j: usize = 0;
    while j < nu {
        checksum += values[j].len() as i64;
        j += 997;
    }
    checksum
}

fn main() {
    println!("{}", run(100000));
}
