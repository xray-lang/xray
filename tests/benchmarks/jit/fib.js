// Node.js (V8): Fibonacci benchmark
function fib(n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

console.log(fib(40));
