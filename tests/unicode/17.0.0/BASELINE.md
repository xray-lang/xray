# Task 199 P0 baseline

Baseline commit: `706d2e09cec1d48ef46b96a80adfa52fad05197f`

Host snapshot (2026-07-15): macOS, AppleClang 21.0.0.21000101, CMake
Release build using Unix Makefiles. These values are evidence for before/after
comparison, not cross-machine performance thresholds.

## Release CLI size before grapheme runtime

```text
build-release-make/xray = 6,297,976 bytes
```

At this baseline there is no linked grapheme property table or cursor.

## Existing rune iterator

Command, repeated five times with the baseline release VM:

```sh
build-release-make/xray run tests/unicode/17.0.0/rune_iterator_baseline.xr
```

The benchmark traverses 12,800,000 ASCII bytes through `string.runes()` and
includes the current heap iterator construction and VM protocol overhead.

```text
elapsed_ns samples:
1,222,733,000
1,297,155,000
1,281,581,000
1,258,303,000
1,350,334,000

median = 1,281,581,000 ns
median throughput = 9.99 MB/s (decimal)
checksum = 1,095,000,000
```

Future grapheme performance reports must record the exact binary, host, corpus,
sample set, and whether the path is the internal cursor, VM iterator, or AOT
direct loop; those lanes are not interchangeable.
