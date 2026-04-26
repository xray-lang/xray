# VM microbench

Measures the cost of the VM dispatch hot loop with a single mixed
workload covering every dispatch category, so that any structural
change to `src/vm/` can be verified to produce no measurable
regression in the computed-goto path.

## Workload

`vm_microbench.xr` runs eight tight loops back-to-back:

| Section | What it stresses | Owning dispatch include |
|---------|-----------------|--------------------------|
| `bench_arith` | `OP_ADD` / `OP_LT` / `OP_JMP` register-form combo | `xvm_dispatch_arith.inc.c` + `compare` + `jump` |
| `bench_bitwise` | `OP_BAND` / `OP_BOR` / `OP_BXOR` / `OP_SHR` / `OP_SHL` | `xvm_dispatch_bitwise.inc.c` |
| `bench_array` | `OP_ARRAY_PUSH` / `OP_ARRAY_GET` / `OP_ARRAY_LEN` | `xvm_dispatch_collection.inc.c` |
| `bench_map` | `OP_MAP_SET` / `OP_MAP_GET` (also exercises invoke-IC) | `xvm_dispatch_collection.inc.c` |
| `bench_string` | `s.length` / `s.startsWith(...)` (invoke-IC monomorphic site) | `xvm_dispatch_object.inc.c` |
| `bench_calls` | `OP_CALL` / `OP_RETURN` round-trip | `xvm_dispatch_call.inc.c` |
| `bench_field_ic` | `OP_GETFIELD_IC` / `OP_SETFIELD` on a class instance | `xvm_dispatch_object.inc.c` |
| `bench_method` | `OP_INVOKE` monomorphic class method dispatch | `xvm_dispatch_object.inc.c` |

Iteration counts (`nHot`, `nMid`, `nLow`) are sized so each section
contributes ~10× the process startup cost, ensuring wall-clock
measures dispatch work rather than `xray run` boot.

## Reproduction

```bash
# Build release
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 8

# Single run
build-release/xray run bench/vm_microbench.xr

# Best-of-N comparison against another binary
for i in 1 2 3 4 5 6 7 8 9 10; do
  /usr/bin/time -p build-release/xray run bench/vm_microbench.xr >/dev/null
done 2>&1 | awk '/^real/ {print $2}' | sort -n
```

Take the **median** as the headline number. The min is more
volatile under macOS scheduler noise; the max is dominated by
system jitter (occasional 30-40% outliers when other processes
touch the cache).

## Reference: dispatch split verification

The 16 `xvm_dispatch_*.inc.c` files included from `src/vm/xvm.c`
exist to physically split the dispatch table while keeping a
single `run()` function for the C compiler. The .inc.c approach
was chosen specifically to preserve computed-goto direct
threading; this benchmark is the gate that confirms it actually
did.

Reference comparison run (Apr 2026, 10 iterations each, MacBook,
Release build, no other load):

| Binary | xvm.c lines | min | median | mean | max |
|--------|-------------|-----|--------|------|-----|
| Pre-split (`a4ffe14`) | 7912 | 1.050s | 1.070s | 1.109s* | 1.450s* |
| Post-split (`b7e7e10`) | 784 + 16 inc.c | 1.060s | 1.070s | 1.076s | 1.110s |

\* The pre-split max is a one-off scheduler outlier (other 9 runs
were 1.05-1.09); mean drops back to ~1.07 if discarded.

**Result:** median 0% delta, min within 1.0% (at noise floor), max
strictly better post-split (no outliers). Dispatch table physical
split lands with no measurable performance impact.

If a future change to the VM regresses median by more than 1%
relative to the surrounding commits, that's a real regression
worth investigating; below 1% is below the measurement floor on
this workload.

## `vm_invoke_microbench.xr` (focused invoke-IC bench)

Sibling micro-benchmark targeted specifically at OP_INVOKE_BUILTIN
monomorphic invoke-IC throughput. Each section calls one builtin
method on a single, type-stable receiver in a tight loop, so after
warm-up the IC slot at every call site is locked to
(receiver-type, &XrMethodSlot) and the hot path collapses to one
cmp + one indirect call.

Sections:

| Section | Methods exercised | Receiver type |
|---------|-------------------|---------------|
| `bench_string_methods` | `length`, `charAt` | `String` |
| `bench_array_methods`  | `push`, `length`, indexed get | `Array<int>` |
| `bench_map_methods`    | `set`, `has`, `length` | `Map` |
| `bench_int_methods`    | `toString`, `abs` | `int` (raw-receiver) |

Reference run (Apr 2026, 3 iterations, MacBook, Release build):

```
real 1.31  real 1.28  real 1.31
```

Use this whenever a change touches `xic_builtin.{c,h}`, the
per-type method tables under `runtime/{value,object}/x*_methods.*`,
or the `OP_INVOKE_BUILTIN` dispatch path in
`xvm_dispatch_invoke.inc.c`. Run the same section count before /
after the change; deltas above ~3% on the median signal a real
shift in the IC fast path.
