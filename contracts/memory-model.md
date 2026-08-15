# Memory model contract

Status: frozen with spec §3.0 (evaluation order) and §16.9 (concurrency
memory model).

The spec states what a program may observe. This contract freezes the
implementation obligations that make those statements true, so a change to
either side has to be argued rather than drift.

## Evaluation order

- M1: evaluation order is total and identical on every backend. There is no
  unspecified and no undefined evaluation order in the language. The rules
  are spec §3.0 E1-E11; `src/frontend/canonical/xcanon.c` is where the
  non-trivial ones are realised.
- M2: a place expression in an assignment or compound assignment is evaluated
  exactly once. Where that requires a temporary, the canonicalizer emits a
  value block (`BlockNode.is_canon_value_block`) whose last statement produces
  the value, so the rewrite holds in expression position as well as statement
  position.
- M3: the differential suite is the gate for M1. `tests/diff/cases/semantics/
  evaluation_order/` prints a marker per subexpression, so its stdout is the
  evaluation trace, and the VM/AOT comparison asserts both backends produce
  it. Weakening M1 would silently retire this gate: an order declared
  unspecified turns a backend divergence into a non-defect.

## Synchronisation edges

- M4: the edges of spec §16.9.2 are exhaustive, and the non-edges of §16.9.3
  are denied just as strongly. Adding a runtime mechanism that programs may
  synchronise through requires adding it to that table.
- M5: every edge is realised by a C11 acquire/release pair or by a lock in
  `src/coro/`. Channel buffer and wait-queue mutation happens under
  `XrChannel.lock`; task completion publishes with a release store that the
  awaiting side consumes with an acquire load.
- M6: `xisa/xi/ops.def` declares each op's edge in its `:sync` column. Where
  the edge's strength is chosen at run time by an `Ordering` argument, the
  declared value is the strongest edge the op can carry - the fail-closed
  upper bound, because the argument is not always a compile-time constant.
- M7: an op declaring a `:sync` edge is neither speculatable nor
  value-numbered. Duplicating a barrier invents an edge and CSE-ing two of
  them removes one; both are rejected by the generator, not by review.

## Optimiser obligations

- M8: alias disjointness is not a licence to reorder. No pass may move an
  ordinary memory operation across an op that carries a `:sync` edge or that
  may suspend, however disjoint their TBAA groups are.
  `xi_op_is_ordering_barrier()` is the only query for this; a pass must not
  re-derive it from effect flags.
- M9: `XI_MEM_NONE` means "touches no memory". It is not a fallback for
  memory the lattice cannot name - alias queries answer no-alias for it, so a
  store carrying it stops killing loads. ops.def rejects any op that declares
  a memory effect without a `:tbaa-group`, which makes the trap unreachable
  from the single source of truth.
- M10: `XI_MEM_FRESH` claims that only the allocating op can name the storage.
  An op may carry it only if every write it performs targets storage it
  allocated itself; that is why it is rejected in combination with a
  `memory-read` effect.
- M11: `XiValue.mem_group` is always the op's declared group, or its
  FIELD -> FIELD_ID refinement. A pass that rewrites an op in place
  reassigns the group from `xi_tbaa_group_for_op()`; the verifier checks the
  value against the table rather than against a list of exempt ops.
  Mutable capture cell reads/writes are explicit `xi.cell.get` /
  `xi.cell.set` operations in the `upval` group. Weak field promotion/storage
  are explicit weak-field operations; neither meaning may be recovered from a
  lowering flag or backend register state.
- M11a: GVN/PRE may not synthesize a new ownership root: an expression whose
  result needs ARC is not a PRE candidate. Full redundancy may replace an
  explicit-RC value only with `XI_COPY(VALUE_CLONE)`, after atomically clearing
  every opcode-specific plan/evidence field; the clone is the new owner and
  the eliminated expression contributes no stale effect, intrinsic, or alias
  identity.

## Reference counting

- M12: an object reachable by two execution agents has an atomic reference
  count. The promotion points are exhaustive (spec §16.9.4) and apply deeply
  to the reachable graph. Concurrent retain/release on a non-atomic count is
  memory corruption, not a lost update, so this is a correctness obligation
  rather than an accuracy one.

## Guarantee boundary

- M13: the data-race-freedom theorem (spec §16.9.5) holds for the safe
  subset and names its escape hatches. The statement and the list move
  together: adding a capability that can introduce a data race means adding
  it to the list in the same change, and bringing one inside the guarantee
  means naming the mechanism that does so.

## Evidence

A change to these rules re-runs and, where the expected output moves, updates:

- `tests/diff/cases/semantics/evaluation_order/` - evaluation-order traces
- `tests/diff/cases/semantics/optimizer/` - reordering regressions that only
  appear once the Xi passes run, so they need the `-O2` differential lane
- `tests/concurrency_litmus/` - message passing, store buffering, IRIW and
  Dekker, run under AOT `-O2` where the Xi optimiser is active
- `tests/unit/ir/test_xi_tbaa.c` - op-table invariants (M7, M9, M10, M11)
- `tests/unit/ir/test_xi_licm.c` - barrier obligations (M8)

## Digest anchors

anchor-sha256: xisa/xi/ops.def 6632a2d9bff8cf827c51aa905d125f06a3f2e822ed414edad02202cc73191f52
anchor-sha256: src/ir/xi_tbaa.c 304e00919092f45875e76dc0d9e958bc706ecc8cf0f0b89ed80d51392276f6f4
anchor-sha256: src/ir/xi_tbaa.h 3f361c253c2a6073043d9a8c77f100cf3b51bc16077fbff3f6f3a381d634c027
anchor-sha256: src/ir/xi_opt_licm.c 06f7494501b87db38c4426987c7f8242451624d10686309e100f3ca66dac819f
anchor-sha256: src/ir/xi_opt_gvn_pre.c 1f4bd9cb11456fc45547c485381b1a49a46985c0bbfacf586970612e037d2def
anchor-sha256: src/ir/xi_memssa.c 622da949ed61de3f085e43cbf7f11c6f43ef78ff56714a5b384a2b2174598be7
anchor-sha256: src/coro/xchannel.c 4ac92d6b0cc987bc5c2809812b6728843231f0c6887f0dde2595f532e474a943
anchor-sha256: src/coro/xtask.c a58cd9d324c5919b8e60b2130523ee3c544ba600fe36be0fe3a93232faadb0ba
anchor-sha256: src/coro/xtask_await.c 1d6026df35f12ff155091a2ee0e54ed46f094e34ee6af89df011382028bd9708
anchor-sha256: src/frontend/canonical/xcanon.c 05874f13d6e736744f77512423d127e811144afd1dbd16a800bb8e570339dfb7
