# Blocker: product program authority admits one toy graph, so a stdlib module cannot gain an Xray body without losing AOT

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`
- **Requested owner**: H (AOT / program semantic closure)
- **Severity**: blocks the lane's whole mandate. Every module that gains an
  `.xr` semantic source enters the module graph, and the graph is refused.

## Exact source identity

| item | value |
|---|---|
| base commit | `f78ca940aeecd8d2512520a46d5e3391ec75b117` |
| worker branch | `work/a-stdlib-selfhost-r2-f78ca940a` |
| parked migration | `work/a-stdlib-time-selfhost-parked-f78ca940a` at `d3fc9ad078569d79769b1d7d7b864650c0adeba0` |

## The measurement

One source, two binaries built from the same base:

```xray
import time
var n = time.now()
print(n)
```

| binary | result |
|---|---|
| base, `time` still whole-module native | generated C, links, runs, prints `1787884792853` |
| parked branch, `time` owns its semantics in `stdlib/time/time.xr` | `XR_TARGET_1000: product TargetPlan requires one canonical program authority` |

Nothing about the reading changed. The refusal is a consequence of the module
count: with no Xray body, `time` never entered the graph and the program was a
single module.

`tests/aot/filetests/link/runtime_time_symbol.xr` is the committed case that
records the capability, and it is the one and only new failure the migration
causes. Measured by running the link filetests on both trees and diffing the
failure sets: base fails 6, the parked branch fails 7, and the difference is
exactly that case. `system_time_queries.xr` already fails on the base.

## Where the refusal is decided

`src/aot/xaot_driver.c:2565`

```c
if (!source_program_closure && nmodules != 1) {
    aot_bundle.error_msg =
        "XR_TARGET_1000: product TargetPlan requires one canonical program authority";
```

So a multi-module program is admitted only when
`xa_program_semantic_closure_publish_scalar_module_graph` published a closure.
That predicate (`src/frontend/analyzer/xa_program_semantic_closure.c:1097`)
accepts exactly one program shape:

- the graph holds exactly 2 modules, no cycle, `topo_count == 2`
- the entry module's syntax has exactly 2 statements: one import, one function
- the dependency module's syntax has exactly 1 statement: one exported function
- the import names exactly 1 member and carries no alias
- the entry function's body contains exactly 1 call, with exactly 1 argument,
  no default arguments and no type arguments
- the dependency function's body contains no call at all
- both functions and the imported symbol pass a scalar-only exactness check

This is an acceptance for a single hand-written toy, not a two-module
capability. `stdlib/time/time.xr` fails it on the first condition it reaches:
it declares seven exported functions where the predicate admits one.

## Why this is the lane's blocker and not one module's

Measured on the base binary, one probe per module:

| module | AOT today |
|---|---|
| `time` | generated C |
| `mem` | generated C |
| `runtime` | refused, `XR_TARGET_1003` |
| `math` | refused, `XR_TARGET_1003` |
| `os` (already `.xr`) | refused, `XR_TARGET_1000` |

Modules that already own their semantics in Xray are already refused. So the
predicate is what stands between the standard library and AOT, and it will
refuse every module the lane migrates from here on. For `time` and `mem` the
migration additionally converts a working capability into a refusal, which is
why the `time` work is parked rather than merged.

## Requested capability

Admit a module graph whose modules are ordinary Xray modules: many exported
functions, many statements, many calls per body, calls with more than one
argument, and non-scalar types. The narrow scalar predicate can remain as a
fast path, but the general graph needs an authority that is derived rather
than pattern-matched against one shape.

## What the parked branch already contains

The `time` migration is finished, tested and pushed, waiting on this capability:

- `stdlib/time/time.xr` owns all seven clock readings; the C side answers only
  realtime, monotonic and CPU nanoseconds plus the UTC offset at a given second
- unit scaling and `localOffset() == localOffsetAt(now() / 1000)` are Xray, so
  each is stated once instead of once per backend
- `tests/regression/10_stdlib/1199_time_units.xr`, 6 cases, passes on the VM
- the obsolete AOT recognizer is removed: `xi_cgen` no longer holds a table of
  the seven public time names, and the five dead `xr_aot_time_*` wrappers are
  gone
- `contract_freeze` PASS (28), `check_semantic_owners` PASS,
  `check_stdlib_aot_helper_residue` PASS

It can be merged unchanged the moment a real module graph is admitted.

## Files deliberately not modified

```
src/aot/xaot_driver.c
src/frontend/analyzer/xa_program_semantic_closure.c
```

Both are the unified target-machine lane's. The standard-library lane can
supply the module corpus and the per-module probes once the graph predicate
exists.

## What the lane did instead

Moved to `runtime`, which the probe above shows is already refused by AOT, so
migrating it costs no capability.
