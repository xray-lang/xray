# Blocker: a local heap variable rebound inside a loop has no AOT representation schema

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`
- **Requested owner**: H (AOT representation refinement)
- **Severity**: this is not a standard-library problem. `var s = ""` followed by
  `for (...) { s = s + "x" }` inside a function cannot be compiled to native
  code. That is the most ordinary string-building loop there is.

## Exact source identity

| item | value |
|---|---|
| base commit | `f78ca940aeecd8d2512520a46d5e3391ec75b117` |
| worker branch | `work/a-stdlib-selfhost-r2-f78ca940a` |
| command | `xray build --native -O 2` |

## Minimal case

Two lines, no standard library beyond the prelude:

```xray
fn run() -> i64 {
    var s = ""
    for (var i = 0; i < 5; i++) { s = s + "x" }
    return len(s)
}
print(run())
```

```
Error: module representation authority build failed:
  XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE value=26 operation=25
```

The same refusal with no string involved, and no function call at all:

```xray
fn run() -> i64 {
    var s = Array<u8>(16)
    for (var i = 0; i < 3; i++) { s = Array<u8>(16) }
    return s[0] as i64
}
```

## What the refusal is keyed on

Each row below was built on its own. The refusal is the same
`XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE` in every refused row.

| shape | result |
|---|---|
| local `Array<u8>` rebound inside a loop | refused |
| local `Array<i64>` rebound inside a loop | refused |
| local `string` rebound inside a loop | refused |
| local `Map<string, i64>` rebound inside a loop | refused |
| local `Array<u8>?` rebound inside a loop | refused |
| local array passed as a `ref` argument inside a loop | refused |
| **local `i64` rebound inside a loop** | passes |
| **the same rebinding at top level rather than in a function** | passes |
| **the same rebinding twice, without a loop** | passes |
| **element writes into a loop-carried local array** | passes |
| **`push` onto a loop-carried local array** | passes |
| **reading a loop-carried local array** | passes |
| **the local declared inside the loop body, fresh each round** | passes |
| **the local declared inside the loop body, `ref` argument** | passes |

So the missing schema is for exactly one thing: a **function-local place of heap
representation whose binding is loop-carried**. A scalar in the same position is
fine, the same variable at module scope is fine, and a fresh binding per
iteration is fine.

Loop count does not matter: one iteration is refused just as three are. Neither
`for` nor `while` is special; both refuse.

## Two things it is not

- Not `ref`. The value-returning form of the same code refuses identically, and
  a `ref` argument passes when its referent is declared inside the loop body.
- Not `StringBuilder`, which is a separate gap: a function-local
  `StringBuilder` refuses with the same code whether or not a loop is involved,
  and whether or not `toString` is called. Recorded here only so it is not
  mistaken for this one.

## Why the lane hit it

`crypto.xr` compiles its AES-256 block transform over a state array that is
carried across the round loop. That is what a block cipher is. The lane found
this while trying to answer whether an Xray standard library costs AOT
performance, and could not measure the cipher at all.

The answer for the part that does compile is worth recording: the same Xray
SHA-256 kernel over 4 KB runs at **1329 us interpreted and 14 us compiled**,
against **16 us** for the C implementation it replaced. Compiled Xray is at
parity with hand-written C. The cost of a self-hosted standard library is a
development-time interpretation cost, not a shipped-binary cost -- which makes
this refusal the thing standing between the two, rather than a performance
question about Xray.

## A shape that does compile

The whole CBC-plus-block-cipher structure compiles when the round loop only
element-writes the carried state, every temporary is declared inside the loop
body, and no helper is called with `ref` from inside a loop:

```xray
fn block(rk: Array<u8>, input: Array<u8>) -> Array<u8> {
    var state = Array<u8>(16)
    for (var i = 0; i < 16; i++) { state[i] = input[i] }
    for (var round = 0; round < 14; round++) {
        for (var i = 0; i < 16; i++) { state[i] = SB[(state[i] as i64) & 3] }
        var t = Array<u8>(16)                       // fresh each round
        for (var i = 0; i < 16; i++) { t[i] = state[i] }
        ...
    }
    return state
}
```

This is a usable route for the cipher, at the cost of inlining what were four
named round steps into one loop body. It is not a route for the general case:
nothing can be done for `s = s + x` except write it a different way, and a user
should not have to know that.

## Root cause

`XRAY_AOT_REFINE_TRACE=1` names it exactly:

```
[aot-refine] refused in the use-site oracle: the use site admits no storage for
             this operand: its opcode branch in oracle_use_storage names no
             family that covers the value
[aot-refine]   value=7      defined by operation 5 CONST, definition rep = tagged
[aot-refine]   operation=7  the USE SITE = opcode PHI, operand 0, block 2
```

The use site is the loop header's **PHI**. A binding carried across iterations
becomes a phi, which is why every passing row above is a shape that produces no
phi for the value: a fresh binding each round, a module-scope slot, an element
write that leaves the array's identity alone.

`oracle_use_storage` does have a phi branch
(`src/aot/refine/xr_aot_representation_refinement.c:9723`):

```c
case XI_PHI:
    if (ctx->policy->force_phi_tagged) {
        *out_storage = XR_REP_TAGGED;
        return true;
    }
    return oracle_machine_storage(ctx, operation->result_value, out_storage, &ignored_kind);
```

so the gap is one level below it: `oracle_machine_storage` names no family for
the phi's result when that result is heap-represented. Both AOT policies set
`force_phi_tagged = false` (`src/ir/xi_opt.h:94` and `:102`; only
`xi_rep_policy_tagged_boundary` sets it true), so the fallback branch is the one
that runs, and it has nothing to answer with.

A scalar phi passes because the scalar family claims it. Nothing claims a phi
whose result is an array, a string or a map.

## Requested capability

Either a storage family that claims a heap-represented phi result, so the value
stays native across the merge, or a fallback that lets such a phi be tagged
under the AOT policies at the cost of one box/unbox at the merge point. The
first is the real answer; the second would unblock ordinary code immediately.

A source position in the diagnostic would also help: the refusal names `value`
and `operation` ordinals, and reaching the shape above took a bisection rather
than a reading.

## Files deliberately not modified

```
src/aot/refine/**
```

The lane can supply the case matrix above as a corpus once the schema exists.
