# Blocker: 73 ctest timeouts are literals the sanitizer scale never reaches

- **Lane**: 8 (test discipline wiring), round 3
- **Status**: `PARTIAL` — five entries fixed in this branch, the family not
  swept.
- **Severity**: it manufactures red. A threshold nobody measured turns a
  passing test into `***Timeout`, which reads as a failure, and the cheapest
  response to a lane that fails at random is to exclude it.

## The mechanism

`CMakeLists.txt:879-886` states the policy and explains itself:

```cmake
# Sanitizer builds (ASan/UBSan/TSan/MSan) run compile-heavy stdlib/AOT tests
# 5-9x slower, so scale their CTest timeouts to avoid spurious sanitizer-only
# timeouts. Non-sanitizer builds keep the tight timeouts as a real perf guard.
if(ENABLE_SANITIZERS OR ENABLE_TSAN OR ENABLE_MSAN)
    set(XR_TEST_TIMEOUT_SCALE 8)
else()
    set(XR_TEST_TIMEOUT_SCALE 1)
endif()
```

The mechanism is right. The problem is reach: **164 registered tests carry a
literal `TIMEOUT`, and only a minority multiply by `XR_TEST_TIMEOUT_SCALE`.**
78 of the literals are exactly `30`. For those, a sanitizer build gets the same
30 seconds while running 5-9x slower — so any test taking more than about four
seconds on an idle Release tree cannot finish under ASan.

The value 30 is a family default, not a measurement. `query_surface_residue`
(2752) and `string_surface_residue` (2780) sit 28 lines apart with identical
property blocks; they were written from the same template.

## Measured, serially, on an idle Release tree

| test | measured | old ceiling | under ASan (×5–9) |
|---|---|---|---|
| `query_surface_residue` | 16.0–24.4s | 30 | 80–220s |
| `legacy_product_residue_inventory` | 14.2s | 30 | 71–128s |
| `string_surface_residue` | 9.1–22.7s | 30 | 46–205s |
| `exact_scalar_surface_residue` | 4.8s | 60 | 24–43s |
| `c_interop_surface_residue` | 2.5s | 30 | 13–23s |
| `error_effect_convergence_inventory` | 2.2s | 30 | 11–20s |

`query_surface_residue` is the slowest passing test in the whole suite (18.7s
on an idle machine per the round-3 baseline). Its ceiling was 30.

## What this actually caused

Both `query_surface_residue` and `string_surface_residue` were on the CI
exclusion list, with no reason recorded next to either. Measuring them for this
round found them **passing** — 24.4s and 22.7s serially — and separately
observed each reporting `***Timeout 30.07` under parallel load. The round-3
post-merge sweep reports `string_surface_residue ***Timeout 30.07` as well.

So the causal chain is complete, and it is a loop:

> a threshold set by template → the test fails at random → excluding it is the
> cheapest fix → the exclusion list gains a line nobody can explain → the
> exclusion list loses credibility.

A gate's own failure mode manufactured another gate's failure mode.

## Fixed here

Five entries, converted to `math(EXPR ... "N * ${XR_TEST_TIMEOUT_SCALE}")` with
the measurement written next to each: `query_surface_residue`,
`string_surface_residue`, `legacy_product_residue_inventory`,
`c_interop_surface_residue`, `error_effect_convergence_inventory`.
`query_surface_residue` and `string_surface_residue` were also deleted from
`tests/ci_exclusions.txt`, restoring their blocking coverage.

## Not fixed: the other 73

Two ways to close it, and the second is the one worth doing:

1. Sweep the remaining literals. Mechanical, and it goes stale the moment
   somebody adds a test.
2. **Gate the ratio.** A meta-check that fails when any test's measured time
   exceeds some fraction of its own ceiling would make this class impossible to
   reintroduce. The data already exists — ctest reports per-test durations, and
   `scripts/t.py:6-21` already keeps a hand-maintained cost table that has gone
   stale at least once (the integrator found `aot_incremental_cache` annotated
   `~119s` while it actually measures 163–166s, with both its `COST` and
   `TIMEOUT` derived from that stale figure).

Option 2 subsumes the cost table too: one source, measured rather than
remembered, and it would have caught every row in the table above.

## Adjacent finding

Several of these tests are `_residue` / `_convergence` governance checks that
walk the whole tree. HEAD (`00f665c5c`, "Stop the governance walks from
descending into build trees") already fixed two of them for taking 29s and 127s
on a tree with thirteen build directories. The same walk cost is what puts this
family close to its ceiling in the first place; a developer with more build
directories than the CI runner will hit these timeouts sooner than CI does.
