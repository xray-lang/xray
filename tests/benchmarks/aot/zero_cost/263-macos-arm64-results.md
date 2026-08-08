# Structural object / JSON boundary benchmark evidence (macOS arm64)

## Qualification

- Result: **insufficient**. Construct/destroy, typed parse, and direct
  exact-object stringify pass the frozen 1% regression gate. Dot access,
  static-string bracket access, and scalar widening each have at least one 95%
  bootstrap interval that crosses the gate, so they are not qualified as pass
  or fail.
- Scope: six inherited zero-cost probes only. This run does not cover the full
  workload inventory frozen by task 263 and therefore is not a complete
  performance qualification for the feature.
- Baseline source: `a296ce5837ebb13db8b3bcafffbfb65740dd2c1f`.
- Candidate source: the task-263 worktree based on the same revision, including
  the structural-object / JSON.Map boundary changes recorded by the adjacent
  raw result and binary hashes below.

## Host and method

- MacBook Pro `Mac17,9`, Apple M5 Pro (18 cores), 64 GB RAM.
- macOS 26.5 (`25F71`), Darwin 25.5.0, arm64.
- Apple clang 21.0.0 (`clang-2100.1.1.101`).
- 3 warmups, 30 alternating baseline/candidate pairs, 3 executions per sample.
- Each execution verifies stdout. Timing uses `perf_counter_ns`; confidence
  intervals use 10,000 paired bootstrap resamples.
- Peak RSS is sampled three times with `/usr/bin/time -l`. CPU affinity is
  `unsupported-by-host`, which is material for these short-running probes.
- Gate: the upper bound of both median and p95 candidate/baseline 95% intervals
  must be at most `1.01`.

Raw samples and machine-readable classifications are in
[`263-macos-arm64-results.json`](263-macos-arm64-results.json).

## Timing results

| Probe | Baseline median (ms) | Candidate median (ms) | Median ratio 95% CI | Baseline p95 (ms) | Candidate p95 (ms) | p95 ratio 95% CI | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Exact object dot access | 17.102 | 17.329 | 1.0022–1.0304 | 18.167 | 18.169 | 0.9842–1.0257 | Insufficient |
| Exact object static index | 17.447 | 17.643 | 0.9967–1.0186 | 18.164 | 18.135 | 0.9836–1.0231 | Insufficient |
| Construct / destroy | 207.005 | 199.471 | 0.9586–0.9744 | 209.930 | 207.725 | 0.9665–1.0075 | **Pass** |
| JSON scalar widening | 5.923 | 5.958 | 0.9820–1.0300 | 6.131 | 6.201 | 0.9935–1.0516 | Insufficient |
| Typed JSON parse | 335.705 | 336.072 | 0.9926–1.0063 | 352.533 | 347.885 | 0.9645–1.0014 | **Pass** |
| Exact object stringify | 364.379 | 257.621 | 0.7015–0.7130 | 375.065 | 263.054 | 0.6954–0.7365 | **Pass** |

The access and scalar probes are short enough that host scheduling noise
dominates their narrow effect sizes. None has a 95% interval wholly beyond the
1% regression limit, so the runner reports `insufficient`, not a regression
failure.

The stringify probe compares the former
`Json.stringify(Json.encode(exactObject))` path with the task-263 contract's
direct `JSON.stringify(exactObject)` path. This is intentional: exact objects
must not be materialized as `JSON.Object` merely to serialize them. Explicit
`JSON.value(exactObject)` followed by stringify is covered separately by the
AOT ARC filetest and is not used to claim this direct-boundary result.

## Image and memory results

| Probe | Baseline image (bytes) | Candidate image (bytes) | Baseline peak RSS (bytes) | Candidate peak RSS (bytes) |
| --- | ---: | ---: | ---: | ---: |
| Exact object dot access | 90,376 | 90,168 | 1,490,944 | 1,490,944 |
| Exact object static index | 90,384 | 90,176 | 1,490,944 | 1,507,328 |
| Construct / destroy | 90,384 | 90,176 | 1,523,712 | 1,523,712 |
| JSON scalar widening | 90,232 | 90,008 | 1,490,944 | 1,490,944 |
| Typed JSON parse | 108,032 | 107,776 | 1,556,480–1,572,864 | 1,556,480–1,572,864 |
| Exact object stringify | 109,736 | 92,168 | 227,082,240–227,131,392 | 82,116,608–82,165,760 |

The stringify memory reduction also verifies that the candidate releases the
temporary JSON value and result string. The generated-C ARC contract is frozen
by `json_value_stringify_arc_owned.xr` / `.expect`.

## Native image identities

| Probe | Baseline SHA-256 | Candidate SHA-256 |
| --- | --- | --- |
| Exact object dot access | `7831d55c85393da186c3693544b330ffcb140b5844e55dc5d45f6dc92682afbf` | `32fbd53e62df8e0fc358571cf3c7a3ae1b13ec4ce1678c8641dae44e5bff7e1d` |
| Exact object static index | `63a8640724858df51458c1813921bec4b4298a974c67ae8d68f6d2c4f29159b8` | `0bb21f832883c9895482d32d1fba092e9e1fe85d1f9754e51abbad83e139944f` |
| Construct / destroy | `537d58f6560185048b021ea7e67627775499a24d8e56101ff1a97fed9a741f80` | `56b9970faa5448e2a10294f58d52cd7303e5e8951f06f3f2b7baed1413802d33` |
| JSON scalar widening | `7d0b81ae1a6ce7ca0d3487584e9d273e885f1c226f23bb14d17d71a31e2fc164` | `43a33c7955c4c714a6acd039b8dfda74766280eaf7a77f694e905f027a01a45d` |
| Typed JSON parse | `773f8eb68bfd5832a5ecd2d48033003ff956de1aeebfa402eb5e50a67c9e0109` | `3c79125c65de5d20b88f8c9d3191eb706b388543024b731b1904f609323e2536` |
| Exact object stringify | `e77767d4fc24473eb7d2d0b566f28e85bff5782ee8b2d0136b176cd485f9e98c` | `b5c6667ff7dda1915199d94725a905394a24039a0b41b980136eed40a8201689` |
