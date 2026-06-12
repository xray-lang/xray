// Rust reference for array_indexof_string.xr (safe, iter().position with
// content equality — same semantics as Xray Array<string>.indexOf).

fn run(n: i64, queries: i64) -> i64 {
    let mut haystack: Vec<String> = Vec::new();
    let mut i: i64 = 0;
    while i < n {
        haystack.push(format!("entry-{}", i));
        i += 1;
    }

    let mut found: i64 = 0;
    i = 0;
    while i < queries {
        let needle = format!("entry-{}", (i * 13) % (n * 2));
        if let Some(idx) = haystack.iter().position(|x| *x == needle) {
            found += idx as i64;
        }
        i += 1;
    }

    found
}

fn main() {
    println!("{}", run(2000, 3000));
}
