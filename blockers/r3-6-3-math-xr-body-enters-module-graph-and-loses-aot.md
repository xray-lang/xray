# Blocker: giving `math` an Xray body moves its callers behind the multi-module program authority

- **Lane**: 6-3 (math intrinsic migration, round 3)
- **Status**: `BLOCKED` on lane 4's multi-module program authority
- **Requested owner**: whoever owns `XR_TARGET_1000` / the product TargetPlan
  program authority (`src/plan/target/`), not this lane
- **Severity**: one differential case lost its AOT verdict; eight AOT link
  filetests state properties that cannot be re-derived until this lands.

## What changed

`stdlib/math/math.xr` replaced the `module math { … }` block in
`stdlib/defs/core.def`. Before the migration `import math` bound a whole native
module and the program never entered the module graph, so `math.randomInt(7, 7)`
reached AOT as a direct call. Afterwards `math` is a real module, and a program
importing it needs one canonical program authority that the target planner does
not yet build.

This is the same shape lane 6 recorded for `mem` in round 3
(`blockers/r3-6-xray-wrapper-over-private-leaf-has-no-exact-target-authority.md`):
the case did not change, what changed is that code which used to bypass AOT now
has to pass through it.

## The measurement

Base `34be0379c`, build `-DXRAY_STDLIB_VM_FASTPATHS=OFF`. Three cases that all
import `math` give a byte-identical first refusal:

```
$ ./build-nofp/xray build --native -c -o /tmp/x.c <case>.xr
Error: product Program TargetPlan build failed: XR_TARGET_1000: product TargetPlan requires one canonical program authority
```

| case | before | after |
|---|---|---|
| `semantics/modules/native_module_scalar_call_target.xr` | comparable | **refused** |
| `semantics/stdlib/math_random_system_direct.xr` | already refused on this base | refused |
| `semantics/stdlib/math_core_direct_type_preserve.xr` | listed refused | refused |

`native_module_scalar_call_target.xr` is the only line this change adds to
`tests/diff/known_failures_not_comparable.txt`. The whole differential run moved
from 538 not-comparable to 527, so coverage grew overall; this one case is the
exception and it is recorded rather than hidden.

The refusal is not specific to `math`. On the same build, `import mem`,
`import simd` and `import codegen` refuse identically, and so does the
freestanding hook provider probe for `prelude`. Every already-migrated module is
in this state.

## Deletion condition

Delete the `native_module_scalar_call_target.xr` line from
`known_failures_not_comparable.txt`, and re-derive the eight `*math*` `.expect`
files under `tests/aot/filetests/link/`, when
`xray build --native -c` accepts a program that imports one stdlib module —
that is, when this command stops printing `XR_TARGET_1000`:

```
$ printf 'import math\nprint(math.sqrt(81.0))\n' > /tmp/p.xr
$ ./build-nofp/xray build --native -c -o /tmp/p.c /tmp/p.xr
```

## The eight expectations left un-derived

They are not edited here, because the values cannot be measured on this base and
guessing them would freeze a number nobody checked:

| file | what no longer holds |
|---|---|
| `core_math.expect` | `"stdlib_symbols"` pins 35 public `math.*` names; `math` publishes no `.def` symbols now |
| `core_math_constants.expect` | same, for the 7 constant names; the constants fold from `math.xr` instead |
| `core_math_single_symbol.expect` | `"stdlib_symbols": ["math.sqrt"]` |
| `system_math_random.expect` | `"stdlib_objects"`/`"stdlib_symbols"` name `math.random`/`math.randomInt`; the leaves are `math.__random`/`math.__randomInt` |
| `freestanding_math_allowlist.expect` | `c_contains=XR_FROM_FLOAT(3.14159265358979323846)`: an `.xr` constant folds through `emit_c_float_literal`, which writes `%a`, so the same value now reads `0x1.921fb54442d18p+1` |
| `freestanding_math_constants.expect` | same line |
| `freestanding_math_int_core.expect` | still correct as written; it gained an f64 function whose emission is unverified |
| `core_math.expect` (`c_contains=` block) | the 26 libm call spellings should still appear, but nothing on this base can confirm it |

The `%a` change is a spelling change, not a precision change: `0x1.921fb54442d18p+1`
is the exact double nearest pi, and round-trips where the 20-digit decimal is
only nearest-representable.
