# Enum static iteration compiler/code-size benchmark

This benchmark is the reproducible 8/64/1024-variant matrix for task 210. It
generates every fixture in a temporary directory, builds optimized stripped
native binaries, and records:

- source, stripped bytecode, generated-C, and binary byte sizes;
- compiler wall time and peak resident memory;
- Mach-O `__text`, `__const`, and `__cstring` sizes when available;
- manual ordinal, direct unit-enum value, and descriptor-ordinal loop time;
- size deltas for an unused enum, `variants.length`, variant names, payload
  field names/type tokens, and an erased descriptor escape;
- generated-C occurrences of descriptor/scalar box construction and forbidden
  Array/iterator/dynamic-reflection paths. The escape fixture also records its
  per-run descriptor allocation count, derived from its finite loop trip count
  after the constructor call site has been verified in generated C.

Run the full matrix:

```sh
python3 tests/benchmarks/compiler/enum_static_iteration/run_benchmark.py \
  --xray build/xray \
  --output build/enum_static_iteration_benchmark.json
```

For a quick smoke run:

```sh
python3 tests/benchmarks/compiler/enum_static_iteration/run_benchmark.py \
  --xray build/xray --counts 8,64 --samples 1
```

The benchmark intentionally does not enforce a machine-independent timing
percentage. Correctness/code-shape invariants are enforced; recorded snapshots
are suitable for same-host regression comparison.
