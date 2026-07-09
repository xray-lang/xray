# AOT Zero-cost Runtime Benchmarks

These cases are small native hot-loop benchmarks for evidence-backed paths from
tasks 170, 171, and 172. They are not default pass/fail performance gates. The
runner verifies each program output, records build/run milliseconds, and leaves
the timing threshold decision to humans or dedicated performance CI.

Run all cases:

```sh
tests/benchmarks/aot/zero_cost/run_benchmark.sh build/xray
```

Useful environment:

```text
XRAY_ZERO_COST_BENCH_CASES=class_dispatch,generic_instantiation
XRAY_ZERO_COST_BENCH_REPEAT=3
XRAY_ZERO_COST_BENCH_N=200000
XRAY_ZERO_COST_BENCH_KEEP=1
XRAY_ZERO_COST_BENCH_OPT=2
```

The runner prints TSV columns:

```text
case    iteration    build_ms    run_ms    output    status
```
