# Blocker: the math module's public surface is int-preserving polymorphic, which `.xr` cannot declare

- **Lane**: 6 (standard-library self-hosting, round 3)
- **Status**: `BLOCKED`
- **Requested owner**: whoever owns the intrinsic registry and the Xi math
  lowering path (`src/frontend/analyzer`, `src/ir`, `src/aot`) — not 5 or 7,
  whose scopes are the semantic/target call families
- **Severity**: blocks the whole `math` module. It is the largest single item
  left in `stdlib_no_public_native_surface` (51 of 171 public native symbols).

## Exact source identity

| item | value |
|---|---|
| base commit | `00f665c5c` (tree `afe293b71`) |
| worker branch | `work/6-stdlib-selfhost-00f665c5c` |
| build | `cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF` |
| binary | `build-nofp/xray`, `xray v0.9.2 (VM+AOT, TLS, arm64-darwin)` |

## The declared signature and the implemented one disagree

`stdlib/defs/core.def` declares every math entry over `f64`:

```
fn abs   { signature: "(x: f64): f64" }
fn min   { signature: "(...args: f64): f64" }
fn clamp { signature: "(x: f64, min: f64, max: f64): f64" }
```

The implementation is polymorphic on the runtime value instead.
`stdlib/math/math.c:396-404` (`math_clamp`) branches on
`XR_IS_INT(args[0]) && XR_IS_INT(args[1]) && XR_IS_INT(args[2])` and answers an
`i64`; the same shape appears in `math_abs`, `math_min` and `math_max`.

Measured on the base binary:

```
$ ./build-nofp/xray run tests/diff/cases/semantics/stdlib/math_core_direct_type_preserve.xr
i64
42
```

`math.abs(-42)` answers `i64`, not `f64`. The differential case that pins this
is named for it — `math_core_direct_type_preserve.xr` — and its comment states
the property directly: the AOT direct calls "must preserve the stdlib runtime's
dynamic value shape for integer-preserving helpers".

The compiler carries the same rule twice more:

- `src/ir/xi_lower_expr.c:2095-2101`, `lower_math_call_preserves_int_args`
- `src/frontend/analyzer/xanalyzer_visitor_call.c:4110-4119`,
  `xa_freestanding_math_call_supported`, which admits `min`/`max`/`clamp` into
  the freestanding profile **only** when every argument is an integer.

## Why an `.xr` body cannot state it

Xray has neither overloading nor a dynamic `any` in the standard library — no
`.xr` file under `stdlib/` uses one. So a migrated `export fn abs` must pick a
single spelling. Both choices lose a property that is under test today:

- `abs(x: f64) -> f64` makes `math.abs(-42)` answer `42.0`, changing the
  observable result of `math_core_direct_type_preserve.xr`, and it promises a
  floating-point path for `min`/`max`/`clamp` under
  `--profile freestanding -nostdlib`, where
  `freestanding_math_allowlist.expect` requires `c_not_contains=fmin(` and
  `c_not_contains=#include <math.h>`.
- `abs(x: i64) -> i64` drops the float half of the surface entirely.

## Three hardcoded tables key on the public names, so renaming breaks the link

The migration shape used by every other module — rename `fn x` to `fn __x`,
wrap it in `.xr` — does not survive here, because `aot: "builtin"` carries no C
expression. The emission is three handwritten tables that all match on the
public name:

| site | what it matches |
|---|---|
| `src/ir/xi_backend_lower.c:42-73` | `backend_math_call_arity_ok` holds 34 public names; the aux is built as `"math." + member` |
| `src/aot/xi_cgen_dispatch_helpers.inc.c:7616-7686` | strips the `"math."` prefix and looks the member up in a second handwritten table |
| `src/aot/xi_cgen_dispatch_helpers.inc.c:3951-3966` | `xicgen_import_ref_is_core_math_member` holds a third copy of the 34 names |

After a rename to `__sqrt`, the arity table no longer matches, no
`XI_CALL_BUILTIN` is produced, and `cg_aot_stdlib_has_direct_member`
(`src/aot/xi_cgen_stdlib_helpers.inc.c:179-199`) only answers for
`aot_kind == "method"` rows, so the call has no emitter at all.

Corroborating count: `stdlib/defs/core.def` holds 123 `fn __` private leaves
and **none** of them uses `aot: "builtin"`. Every one is
`aot: "xrt_*"` with `link_object: true`. The shape math would need does not
exist yet.

## The link contract that a migration would have to renegotiate

`tests/aot/filetests/link/core_math.expect` pins, among others:

```
contains="stdlib_symbols": ["math.sqrt", "math.pow", ... 35 public names]
c_contains=sqrt(
not_contains="stdlib_objects": ["math"]
c_not_contains=xrt_method_
```

`tests/aot/filetests/link/freestanding_math_allowlist.expect` additionally pins
`c_contains=XR_FROM_FLOAT(3.14159265358979323846)` — `math.PI` must fold to a
literal — and forbids `isnan(`, `fmin(`, `fmax(`, `xrt_math_` and
`#include <math.h>`. `stdlib/freestanding_allowlist:12` names math as part of
the freestanding commitment surface guarded by that probe.

## The path that would work

`src/frontend/analyzer/xanalyzer_visitor_decl.c:173-201`
(`xa_bind_registry_intrinsic`) binds an `.xr` declaration to a compiler-owned
intrinsic identity by looking up `<canonical_module>.<member>` in
`src/frontend/analyzer/xa_intrinsic_registry.def` (159 entries). This is how
`simd` and `codegen` are freestanding modules with `.xr` semantic sources
today, and `stdlib/codegen/codegen.xr` says so in its header comment: AOT never
discovers them by module or function spelling.

Making math take the same route means adding roughly 26 `XA_INTRINSIC(MATH_*, …)`
entries plus their lowering and emission, and deciding the polymorphism
question above. Both are `src/` changes outside this lane's file boundary.

## What this lane did instead

Nothing in `stdlib/` was changed for math. The drafted `math.xr`, the drafted
`module math { … }` replacement (28 private leaves, all 14 consts deleted) and
the drafted boundary entry are kept out of the tree; they are ready to apply
once the polymorphism question is decided, and they are recorded here rather
than committed so that no half-migration sits in the repository.

## Unverified items in the draft, for whoever picks this up

- Constant-position `1.0 / 0.0` and `0.0 / 0.0` folding for `INF` / `NAN` has
  no direct precedent in the tree; the fallback is to keep those two in
  `core.def` and list them in `public_native`, the way `time` keeps `sleep`.
- Variadic `f64` rest parameters have no precedent either. `stdlib/_probe/_probe.xr:26`
  demonstrates the `i64` case only.
