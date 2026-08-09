# Structural object / JSON boundary benchmark evidence (macOS arm64)

## Qualification

- Result: **insufficient, with no measured regression**. Of the eleven paired
  probes, six pass the frozen 1% gate, five have a 95% interval crossing the
  gate, and none fail. Four APIs absent from the frozen revision have complete
  candidate baselines and are marked `recorded`, not presented as before/after
  wins.
- Scope: all workloads frozen by task 263 are represented. The earlier
  six-probe report was incomplete and has been replaced by this native-image
  run.
- Frozen source: `a296ce5837ebb13db8b3bcafffbfb65740dd2c1f`.
- Candidate: the task-263 worktree after the JSON loop-consume ARC fix. Every
  executable was built with `xray build --native -O 2`; the runner invokes the
  linked native images directly and records their SHA-256 identities.

`insufficient` is deliberately not upgraded to pass. On this macOS host CPU
affinity is unavailable, and p95 is sensitive to scheduler outliers. The raw
samples still establish that no paired probe has a confidence interval wholly
beyond the 1% regression limit.

## Host and method

- MacBook Pro `Mac17,9`, Apple M5 Pro, 64 GB RAM.
- macOS 26.5 (`25F71`), Darwin arm64.
- Apple clang 21.0.0 (`clang-2100.1.1.101`).
- 3 warmups and 30 alternating baseline/candidate pairs; every execution
  verifies stdout.
- Timing uses `perf_counter_ns`; intervals use 10,000 paired bootstrap
  resamples. Peak RSS is sampled three times with `/usr/bin/time -l`.
- Gate: the upper bound of both median and p95 candidate/baseline 95% intervals
  must be at most `1.01`.
- `json_scalar_widening` passes each loop input through `codegen.opaque`, so the
  C compiler cannot fold the whole loop into the expected result.

Machine-readable samples, classifications, Mach-O section sizes and native
image hashes are in [`263-macos-arm64-results.json`](263-macos-arm64-results.json).

## Timing results

| Probe | Baseline median (ms) | Candidate median (ms) | Median ratio 95% CI | Baseline p95 (ms) | Candidate p95 (ms) | p95 ratio 95% CI | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `exact_dot` | 37.376 | 37.394 | 0.9947–1.0085 | 38.211 | 41.050 | 0.9853–1.0992 | insufficient |
| `exact_static_index` | 37.380 | 37.481 | 0.9936–1.0101 | 37.923 | 38.043 | 0.9873–1.0082 | insufficient |
| `construct_destroy` | 71.482 | 69.138 | 0.9346–1.0085 | 76.428 | 75.578 | 0.9576–1.0124 | insufficient |
| `width_constraint_call` | 248.763 | 52.304 | 0.2088–0.2116 | 254.515 | 53.116 | 0.2055–0.2114 | **pass** |
| `object_spread` | 149.119 | 142.837 | 0.9359–0.9809 | 157.413 | 151.348 | 0.9513–0.9982 | **pass** |
| `json_scalar_widening` | 7.628 | 7.621 | 0.9787–1.0176 | 9.158 | 8.828 | 0.9168–1.1277 | insufficient |
| `json_parse_typed` | 115.779 | 116.715 | 0.9931–1.0197 | 121.275 | 122.874 | 0.9903–1.0364 | insufficient |
| `json_parse_ignore` | — | 139.823 | — | — | 148.426 | — | recorded |
| `json_parse_object_map` | 183.671 | 178.343 | 0.9434–0.9818 | 195.842 | 182.017 | 0.9084–0.9772 | **pass** |
| `json_decode_object` | 153.104 | 85.627 | 0.5522–0.5650 | 159.867 | 87.529 | 0.5434–0.5640 | **pass** |
| `json_parse_with_rest` | — | 214.783 | — | — | 221.800 | — | recorded |
| `json_path_ops` | — | 64.962 | — | — | 67.549 | — | recorded |
| `json_encode_stringify` | 125.132 | 86.860 | 0.6807–0.7147 | 131.389 | 94.135 | 0.6840–0.7457 | **pass** |
| `json_stringify_map` | 79.556 | 71.861 | 0.8885–0.9212 | 84.700 | 75.801 | 0.8700–0.9147 | **pass** |
| `json_stringify_with_rest` | — | 208.129 | — | — | 214.856 | — | recorded |

`json_decode_object` compares the direct API with
`JSON.decode(JSON.value(object))`, both built by the candidate compiler. It is
a same-feature direct-path comparison, not a claim about an API present in the
frozen revision. The other paired rows use frozen pre-263 implementations.

## Image, section and memory results

| Probe | Baseline image (B) | Candidate image (B) | Baseline `__text` (B) | Candidate `__text` (B) | Baseline peak RSS (B) | Candidate peak RSS (B) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `exact_dot` | 90,376 | 90,200 | 33,396 | 33,164 | 1,490,944 | 1,490,944 |
| `exact_static_index` | 90,384 | 90,208 | 33,396 | 33,164 | 1,490,944 | 1,507,328 |
| `construct_destroy` | 90,384 | 90,208 | 33,380 | 33,144 | 1,523,712 | 1,523,712 |
| `width_constraint_call` | 90,632 | 90,376 | 35,000 | 33,328 | 1,490,944 | 1,490,944 |
| `object_spread` | 90,464 | 90,272 | 33,620 | 33,416 | 1,540,096 | 1,540,096 |
| `json_scalar_widening` | 90,552 | 90,344 | 32,140 | 31,924 | 1,490,944 | 1,490,944 |
| `json_parse_typed` | 108,032 | 107,808 | 45,632 | 45,728 | 1,556,480–1,572,864 | 1,556,480–1,572,864 |
| `json_parse_ignore` | — | 107,808 | — | 45,752 | — | 1,572,864 |
| `json_parse_object_map` | 90,712 | 90,792 | 36,108 | 40,928 | 584,548,352–584,564,736 | 1,622,016–1,638,400 |
| `json_decode_object` | 108,016 | 91,328 | 46,076 | 41,232 | 98,435,072–98,467,840 | 98,320,384–98,353,152 |
| `json_parse_with_rest` | — | 107,896 | — | 46,040 | — | 1,622,016 |
| `json_path_ops` | — | 108,272 | — | 46,396 | — | 1,572,864–1,589,248 |
| `json_encode_stringify` | 109,736 | 92,200 | 46,636 | 39,632 | 227,082,240 | 82,067,456–82,116,608 |
| `json_stringify_map` | 92,720 | 92,432 | 42,856 | 42,280 | 146,374,656–146,391,040 | 82,100,224–82,116,608 |
| `json_stringify_with_rest` | — | 110,136 | — | 58,404 | — | 82,198,528–82,247,680 |

## Representation evidence

- Width constraint: the evidence dump records two concrete generic
  instantiations, two specialized bodies, four exact object shapes and two
  `direct_ordinal` accesses. The per-origin clone count is 1, well below the
  existing threshold 64 and global `XR_MONO_MAX_INSTANCES` budget 16,384.
  Despite specialization, the candidate image is 256 B smaller and its
  `__text` is 1,672 B smaller than the former open-row dispatch image.
- Structural object header: on the arm64 AOT ABI the frozen `xrt_json_t` header
  was `XrObjHeader` (16 B) + shape pointer (8 B) + `dynamic_fields` pointer
  (8 B), before the flexible field array. The candidate `xrt_object_t` is the
  same header + shape pointer only: 32 B becomes 24 B, with no replacement
  extension pointer. VM objects continue to use their existing 24 B
  header/class prefix; the dynamic hidden-class transition path is removed.
- Map-backed object: the parse probe has one two-entry `JSON.Object` Map. Its
  `JSON.Value` object arm is the same tagged Map reference; `JSON.value(object)`
  performs no node allocation or field copy. The generated-C and ARC filetests
  freeze this identity/retain contract. Candidate peak RSS stays around 1.6 MB
  instead of the frozen dynamic-object parser's approximately 584.6 MB in this
  repeated parse workload.
- WithRest/path are new APIs, so their throughput, image, section and RSS rows
  are baselines for future regressions. They are not counted as passes.

The overall performance qualification remains **insufficient** until a pinned
host or additional sampling closes the five crossing intervals. There is no
measured paired regression that justifies redesigning the static-object / Map
boundary.
