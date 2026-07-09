# Global Evidence Cache Benchmark

This benchmark exercises the `summary -> evidence -> plan` cache boundary used
by tasks 171 and 172. It records cold, warm, body-change, declaration-change,
dump-enabled, and rebuild timings, plus the evidence cache hit/miss summary
reported by `xray build --native --verbose`.

Run:

```sh
tests/benchmarks/compiler/global_evidence_cache/run_benchmark.sh build/xray
```

Useful environment:

```text
XRAY_GLOBAL_EVIDENCE_BENCH_CASES=class,generic,closure,static,capability
XRAY_GLOBAL_EVIDENCE_BENCH_REPEAT=3
XRAY_GLOBAL_EVIDENCE_BENCH_KEEP=1
```

The output is a TSV file printed to stdout and copied to the temporary work
directory as `results.tsv`. The benchmark does not enforce a timing threshold;
it fails only when the cache hit/miss contract is wrong or a generated program
does not build.
