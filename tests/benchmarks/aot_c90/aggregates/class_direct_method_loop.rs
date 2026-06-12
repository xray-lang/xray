// Rust reference for class_direct_method_loop.xr (statement-level isomorphic, safe std-only).

struct Counter {
    value: i64,
    step: i64,
}

impl Counter {
    fn new(init: i64, step: i64) -> Self {
        Counter {
            value: init,
            step,
        }
    }

    fn bump(&mut self, n: i64) -> i64 {
        let mut i: i64 = 0;
        let mut sum: i64 = 0;
        while i < n {
            self.value += self.step;
            sum += self.value;
            i += 1;
        }
        sum + self.value
    }
}

fn run(n: i64) -> i64 {
    let mut c = Counter::new(1, 3);
    c.bump(n)
}

fn main() {
    println!("{}", run(2000000));
}
