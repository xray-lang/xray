# Blocker: the Xi optimization pipeline miscompiles tail calls, and no gate sees most of it

- **Lane**: B (current census / refusal evidence)
- **Status**: `FIXED` in this branch for the VM side, `PARTIALLY FIXED` for the native
  side. Three defects closed, all the same omission at different layers; see "What was
  fixed". One case still refuses natively, at the next layer down.
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
call (`return choose(n)`), over a unit enum. The source shape is incidental; what matters
is that the inliner chooses to inline a function whose body ends in a promoted tail call,
which the root-cause section below establishes.

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

## Root cause of the VM-side miscompilation

Same opcode, different unmodelled layer. Two passes are jointly required, established by
disabling each of the 23 passes in turn against the minimal case: turning off **either**
`inline` **or** `tail_call` restores correct output, and every other pass is irrelevant.

Dumping `main` after inlining (`XRAY_XI_DUMP=main:inline`) shows what happens:

```
func main() -> void {
    v1  = CONST 1
    v2  = PRINT v1
    v19 = CALL v18 v4
    v20 = TAIL_CALL v17 v19     <-- inlined out of roundtrip
    v6  = PRINT v20             <-- and there is still code after it
    v13 = CALL_METHOD v9 v12
    v15 = PRINT v14
    RET
}
```

`tail_call` correctly promotes `return describe(choose(n))` inside `roundtrip`: there it
really is in tail position. `inline` then copies that body into `main` and **carries the
`XI_TAIL_CALL` opcode across unchanged**, where it is no longer in tail position and three
more statements follow it.

Tail semantics live in the opcode: `xi_emit_tail_call`
(`src/ir/xi_emit_call.c:217`) "always emits `OP_TAILCALL` … the op absorbs the tail
semantics". The VM reaches that instruction mid-function, replaces the frame and returns,
so `main` ends after its first `print` — with exit status 0, because nothing went wrong as
far as the VM is concerned.

`src/ir/xi_opt_inline.c` does not mention `XI_TAIL_CALL` **once**. It neither demotes the
opcode back to `XI_CALL` when the call stops being a tail call, nor counts it in the
inlining cost metric (`:157` counts `XI_CALL`, `XI_CALL_METHOD`, `XI_CALL_METHOD_DIRECT`,
`XI_CALL_BUILTIN` and stops), nor excludes callees containing one.

**Why nothing caught it.** `verify_tail_calls` (`src/ir/xi_verify.c:1717`) checks four
invariants about a promoted tail call — no leftover `XI_FLAG_TAIL`, a callee operand
exists, it does not target its own activation, the callee is function-typed — but never
that the call is **in tail position at all**. The AOT side does check the equivalent fact,
as condition 7 of the conformance gate above, which is why the same malformed IR is
refused there and silently executed here.

## One opcode, three unmodelled layers

Both defects in this document are the same omission at different layers. Occurrences of
`XI_TAIL_CALL` per file:

| file | count | consequence |
|---|---:|---|
| `src/ir/xi_opt_inline.c` | **0** | inlining leaves a tail call mid-function; VM miscompiles |
| `src/ir/xi_arc.c` | **0** | callee unresolved after promotion; ownership balance goes negative |
| `src/ir/xi_opt.c` (representation selection) | 1, unrelated | falls to tagged default; BOX/UNBOX breaks tail position; AOT refuses |
| `src/ir/xi_verify.c` | 10 | four invariants checked, tail position not among them |
| `src/aot/refine/xr_aot_representation_refinement.c` | 6 | handles it correctly, paired with `XI_CALL` |
| `src/ir/xi_emit_call.c` | 1 | emits `OP_TAILCALL` unconditionally |

The opcode was introduced with producers, an emitter and a verifier, but its consumers —
the inliner and the representation stage — were never taught it exists. Fixing only the
conformance gate would leave the VM miscompilation untouched, and vice versa.

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

## The ratchet contradiction, and why fixing the root cause dissolved it

Resolved by the fix below; recorded because the shape will recur whenever a case builds at
one optimization level and is refused at another.

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

There was no baseline state in which both lanes were green, which is the point: the only
two ways out were to fix the compiler or to give `backend_diff_optimized` its own baseline
the way the embedded lane already has one. The fix below took the first, and the case now
builds under optimization, so nothing needs baselining and the contradiction is gone.

Worth keeping in mind rather than solved in general: any future case that builds at one
optimization level and is refused at another lands in the same trap, because these lanes
still share one refusal list while disagreeing about what they compile.

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

## What was fixed

Two changes, both in the Xi layer, both teaching an existing rule about the opcode it did
not know.

**Inlining demotes a copied tail call.** `clone_value` in `src/ir/xi_opt_inline.c` already
cleared `XI_FLAG_TAIL` from every cloned value, with a comment explaining that tail
position is scoped to the callee frame. It cleared the flag form only. The promoted form
is an opcode, so it now demotes `XI_TAIL_CALL` to `XI_CALL` in the same place. The
demotion has to happen after the clone rather than at construction, because
`xi_value_clone_metadata` requires the opcode to still match the source and returns false
otherwise, which would leave a half-built value already appended to the block.

**Representation selection knows the opcode.** `sr_def_rep` and `sr_use_rep` in
`src/ir/xi_opt.c` handled `XI_CALL` and let the promoted form fall to the tagged default,
which asked for adapters around a call that has to stay in tail position. Both now treat
the two forms alike, which is what the AOT refinement layer already did.

Measured on the four affected cases, with Xi optimization enabled:

| | before | after |
|---|---|---|
| `aot=full` | 0 passed, 4 refused | **3 passed, 1 refused** |
| `vm=full` | 2 passed, 2 differential failures | **4 passed, 0 failures** |

The silent miscompilation is gone: `unit_enum_return_value.xr` prints all eight values
under `--xi-opt vm=full`, and the simplest general tail call now builds natively where
before none ever had.

## The ownership imbalance, also fixed

`semantics/functions/string_result_direct_call.xr` failed on both sides with

```
XR_OWN_3001: ownership balance becomes negative
  (owner=... origin=TAIL_CALL func=forward block=4 entry=0 delta=-1
   events=[TAIL_CALL@4:0,TAIL_CALL@4:-1])
```

Same omission, a fourth layer, and the subtlest form of it. ARC itself was not at fault:
it runs before promotion, sees an ordinary call, and correctly inserts nothing, because a
call whose result is returned is a move. The semantic plan then re-asks ARC's contract
functions *after* promotion has rewritten the opcode.

Those functions resolve the callee to read its whole-program return ABI, and a site that
fails to resolve does not fail — it falls back to the weaker contract lowering recorded at
the call site. For a callee returning a string literal that weaker fact is
`BORROWED_STATIC`, so the definition was classified as a borrow worth zero while the
function-level return contract, computed before promotion, still said owned and charged
the return -1.

That asymmetry is why it stayed hidden: it needs a callee whose call-site fact and
whole-program fact disagree. A callee returning `"pos"` reproduces it; one returning
`"pos" + "x"` does not, because then both facts say owned. This corpus contains exactly
one such case.

Four sites did this resolution, each with its own copy of the opcode test, and three knew
only the unpromoted form. They now share one resolver, so the promoted form cannot be
recognized in one place and missed in another.

With this, `vm=full` passes all four cases. On the native side
`string_result_direct_call.xr` now advances to a different refusal
(`XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE`), which is the next layer down and
was not investigated.

## A fix that was tried and reverted

Adding the tail-position invariant to `verify_tail_calls` — requiring a promoted tail call
to be its block's control value, which is what the AOT conformance gate checks — looked
like the natural third change and is **wrong to add as things stand**.

With it, every nested tail call (`return describe(choose(n))`) fails to compile at the
representation stage, because representation selection legitimately places a return
adapter after the call and moves the block's control to it. Measured with the invariant
disabled, the VM executes those same graphs correctly. So the invariant converts working
programs into compile failures, which is a regression, and it was removed rather than
kept behind a flag.

What that experiment did establish is a real disagreement, still open: representation
selection emits a return adapter after a tail call, and the AOT conformance gate refuses
exactly that shape. One of the two is wrong, and deciding which is a design question about
whether a tail call may be adapted at all — it belongs to whoever owns both stages, not to
a verifier tweak.
