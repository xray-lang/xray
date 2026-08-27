# Blocker: the Xi optimization pipeline miscompiles tail calls, and no gate sees most of it

- **Lane**: B (current census / refusal evidence)
- **Status**: `BLOCKED` on capability this lane must not implement
- **Requested owner**: H (compiler / unified target machine)
- **Severity**: highest of anything this lane found. One case produces a **silently wrong
  program** — correct exit status, missing output — rather than a refusal. Three of the
  four affected cases are outside every gate's case list. On the native side the feature
  has a **100% refusal rate**: no general tail call has ever passed its conformance gate.

## Exact source identity

| item | value |
|---|---|
| branch head | `04bd99ff06113fa7d8468a1dcb7458474285bd6a` |
| tree state | clean |
| binary | `build/xray`, commit matches head, `dirty=false`, Release |
| census manifest | `build/current-refusals-5cc0daf38888.json`, sha256 `7b941de9c27f01d6fd0a207e8d4f23b9387d9af1602a51ae7647b8022f419f3b` |

## The correctness defect

`tests/diff/cases/semantics/unit_enum_return_value.xr` under Xi optimization stops
producing output partway through `main`, exits 0, and reports no error:

```
build/xray run <case>                     ->  true|true|true|200|100|300|true|200
build/xray run --xi-opt vm=full <case>    ->  true|true|true|200
```

The last four values are simply absent. This is not a refusal and not a crash: the
program claims success while having skipped work.

Minimal reproduction, 22 lines, distilled from that case:

```xray
enum Color { Red, Green }
fn choose(n: i64) -> Color {
    if (n == 0) { return Color.Red }
    return Color.Green
}
fn describe(c: Color) -> i64 {
    if (c == Color.Red) { return 100 }
    return 200
}
fn roundtrip(n: i64) -> i64 {
    return describe(choose(n))
}
final class Painter {
    shifted(n: i64) -> Color {
        return choose(n)
    }
}
fn main() {
    print(1)
    print(roundtrip(0))
    var p = Painter()
    print(describe(p.shifted(1)))
}
main()
```

```
build/xray run min.xr                    ->  1|100|200
build/xray run --xi-opt vm=full min.xr   ->  1
```

Both ingredients are required, established by subtraction:

- Remove the class and keep `roundtrip`: does not reproduce.
- Keep the class but never call its method: does not reproduce.
- Keep both, drop `roundtrip`: does not reproduce.

So the trigger is a free function whose body is a nested tail call
(`return describe(choose(n))`) coexisting with a called class method that is itself a tail
call (`return choose(n)`), over a unit enum.

## Attribution: the Xi pipeline, not the host C optimizer

Isolated on the four affected cases:

| configuration | result |
|---|---|
| `XRAY_AOT_TEST_OPT=2` alone (generated C at `-O2`) | 4 passed, 0 refused |
| `XRAY_DIFF_XI_OPT=vm=full,aot=full` alone | 0 passed, 4 refused |
| `XRAY_DIFF_XI_OPT=aot=full` | 0 passed, 4 refused |
| `XRAY_DIFF_XI_OPT=vm=full` | 2 passed, **2 differential failures** |

The host C optimization level is irrelevant. The defect is entirely in the Xi pipeline,
and it presents differently on each side: the AOT side refuses to build, while the VM side
builds and produces wrong output.

More precisely, the optimizer's only role is to make `XI_TAIL_CALL` exist. Every
`--xi-opt` pass runs before the SemanticPlan is frozen (`src/ir/xi_pipeline.c:839` versus
`:892`); the graph is rewritten afterwards, by representation selection at `:915`. See the
root cause below.

The two `vm=full` differential failures:

- `unit_enum_return_value.xr` — stdout mismatch, the silent truncation above.
- `string_result_direct_call.xr` — the VM half fails to compile with
  `XR_OWN_3001: ownership balance becomes negative (origin=TAIL_CALL func=forward block=4 delta=-1 events=[TAIL_CALL@4:0,TAIL_CALL@4:-1])`
  while the AOT half builds and runs correctly.

## Every affected case is a tail-call case

Running the census's 142 comparable cases under `-O2` with `vm=full,aot=full`:
**138 pass, 0 differential failures, 4 refused.** All four refusals name tail calls:

| case | diagnostic |
|---|---|
| `semantics/optimizer/pass_tail_call_accumulator.xr` | `XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY` |
| `semantics/recursion/self_recursion.xr` | `XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY` |
| `semantics/unit_enum_return_value.xr` | `XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY` |
| `semantics/functions/string_result_direct_call.xr` | `XR_OWN_3001`, `origin=TAIL_CALL` |

## Root cause of the native-side refusal

Independently traced and then re-verified here. The refusal is not specific to these
cases: **no general tail call has ever passed this gate.**

```
fn g(n: i64) -> i64 { return n + 1 }
fn f(n: i64) -> i64 { return g(n) }
fn main() { print(f(1)) }
```

```
build/xray build --native -O 0 --xi-opt aot=full-inline -c -o /tmp/t.c min.xr
  -> XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY operation=13 target-call=1 function=2 value=15
```

Isolation shows exactly one trigger — the `tail_call` pass itself, at `full` level:

| configuration | result |
|---|---|
| `--xi-opt aot=full` | refused |
| `--xi-opt aot=full-tail_call` (that one pass off) | **passes** |
| `--xi-opt aot=light` / `aot=none` | passes |
| `-O 2` with no `--xi-opt` | passes |

Self-recursion is not an exception that proves the gate works: the promotion pass rewrites
a self tail call into a loop backedge, so no `XI_TAIL_CALL` node exists and the gate
trivially passes over zero records.

**The mechanism.** `XI_TAIL_CALL` is not modelled in Xi's representation-selection stage.
`sr_def_rep` (`src/ir/xi_opt.c:3025`) and `sr_use_rep` (`:3453`) each handle `case XI_CALL`
and nothing else, so a promoted tail call falls into `default: return XR_REP_TAGGED`
(`:3136`). The argument is then boxed and the result unboxed:

```
frozen  :  v3 = TAIL_CALL v2 v0 ; RET v3
live    :  v4 = BOX v0 ; v3 = TAIL_CALL v2 v4 ; v5 = UNBOX v3 ; RET v5
```

The conformance gate requires the return block's control value to still be the tail-call
value itself (`find_live_operation_value`, `src/aot/refine/xr_aot_tail_call_conformance.c:74-114`,
violated condition at `:110`, reported at `:188`). After representation selection the block's
control is `v5`, the UNBOX.

**The gate's verdict is semantically right.** An UNBOX sequenced after the call means the
call is no longer in tail position, so frame reuse cannot hold. The defect is upstream:
the representation stage was never taught about `XI_TAIL_CALL`, so the pipeline produces a
"tail call" that is not one. Before this gate existed the same rewrite presumably shipped
silently — that part is inferred from the code, not measured against an older build.

**This is the two-layer predicate drift pattern.** The AOT refinement layer handles both
opcodes together (`src/aot/refine/xr_aot_representation_refinement.c:9688-9689`:
`case XI_CALL: case XI_TAIL_CALL:`), while the Xi layer's representation rules know only
`XI_CALL`. One question, two implementations, one of them missing a case.

**Why the gate's own tests never caught it.** Its only positive tests build synthetic Xi
graphs (`tests/unit/aot/test_xr_aot_refinement.c:774`, `:1651-1696`). A synthetic graph
does not go through `xi_program_select_reps`, so it never carries the BOX/UNBOX pair that
the real pipeline always inserts. The gate was introduced by `febaad063` (2026-08-15),
which touched 18 files and none of them `src/ir/xi_opt.c`.

The diagnostic names here are also ungoverned: `xr_aot_tail_call_conformance_issue_name()`
returns 12 `XR_AOT_TAIL_CALL_CONFORMANCE_*` strings, none registered in
`contracts/target-machine/diagnostic-codes.toml`, so a refusal naming one still owes a
stable diagnostic code.

**Scope limit:** this traces the native-side refusal. Whether the VM-side miscompilation
above shares this root cause is untested — it is the same feature and the same pass, but
the silent-truncation failure was not traced to the representation stage.

## Why no gate catches three of the four

Only `backend_diff_optimized` runs with Xi optimization enabled, and it runs a fixed list,
`tests/diff/optimizer_cases.txt` (57 cases). Of the four cases above, exactly one
(`pass_tail_call_accumulator.xr`) is on that list. The other three are compiled by
`backend_diff` — but that lane runs without Xi optimization, so it sees them pass.

The result is a coverage hole that no lane reports: a case can be correct at the default
optimization level, miscompiled under Xi optimization, and green everywhere.

This lane's census inherits the hole. `tests/diff/survey_refusals.py` measures with
`aot_opt="0"` and `xi_opt=""`, so its 519 refusals and 142 comparable cases describe the
unoptimized pipeline only. Read the census figure as **138 comparable unoptimized and
under Xi optimization**, not 142.

## The optimized lane cannot be made green by any baseline edit

`backend_diff_optimized` currently reports three refusals that are not baselined; two are
the PHI regressions recorded separately, the third is `pass_tail_call_accumulator.xr`.
That third one cannot be baselined away, because the refusal ratchet is shared.

`run_backend_diff.py` derives the refusal list from the divergence baseline
(`<stem>_not_comparable.txt`). Only the embedded lane sets `XRAY_DIFF_BASELINE`, so
`backend_diff`, `backend_diff_optimized`, and `backend_diff_deterministic` all share
`tests/diff/known_failures_not_comparable.txt`. But `backend_diff` is the only full run
(`full_run = not XRAY_DIFF_CASES_FILE`), and only a full run checks the now-passing side
of the ratchet.

Evaluated directly against the ratchet module, with the measured fact that this case
builds at `-O0` and is refused under Xi optimization:

| baseline state | `backend_diff_optimized` | `backend_diff` (full run) |
|---|---|---|
| case absent (today) | red: new refusal | green |
| case present | green | red: listed case now builds, delete it |

There is no baseline state in which both lanes are green. The lane is red until either the
compiler defect is fixed or `backend_diff_optimized` is given its own baseline the way the
embedded lane already has one.

## Reproduce

```
build/xray run --xi-opt vm=full tests/diff/cases/semantics/unit_enum_return_value.xr
```

For the AOT side of any of the four:

```
XRAY_DIFF_XI_OPT=vm=full,aot=full XRAY_DIFF_SINGLE_CASE=<case> \
  python3 tests/diff/run_backend_diff.py build/xray
```

## Files deliberately not modified

`src/ir/**`, `src/aot/**`, `CMakeLists.txt`, `tests/diff/optimizer_cases.txt`, and every
baseline and allowlist. In particular no case was added to any baseline: the ratchet
contradiction above is reported, not worked around.
