# Concurrency litmus tests

Each program is a classic shared-memory litmus test written in xray. They
check the synchronisation edges of spec §16.9.2 by running a shape whose
forbidden outcome is observable, many times, and asserting the outcome never
appears.

## Why they must run under AOT `-O2`

The VM is an interpreter: it does not reorder. Every failure mode these tests
exist to catch lives in the Xi optimiser, which only runs at `XI_OPT_FULL`
(`-O2`). A litmus run at `-O0`, or on the VM alone, proves nothing about the
memory model. The `concurrency_litmus` ctest builds with `--native -O2` for
exactly this reason.

## Why they iterate

A missing edge is a race, and a race is not deterministic. A single iteration
that happens to pass is not evidence. Each case runs its shape `ITERATIONS`
times and reports the count of forbidden observations, so a regression shows
up as a non-zero count rather than as a flake. Passing is still not proof —
these tests can only refute, never confirm. They are paired with the static
gates (`contracts/memory-model.md`, the op-table invariants in
`tests/unit/ir/test_xi_tbaa.c`), which is where confirmation comes from.

## Cases

| Case | Shape | Edge under test |
|---|---|---|
| `mp_channel.xr` | message passing through a channel | §16.9.2 edge 1: everything before a send is visible after the matching receive |
| `mp_task_await.xr` | message passing through task completion | §16.9.2 edge 5: the task body's last action is visible after `await` |
| `mp_atomic_flag.xr` | message passing through a release/acquire flag | §16.9.2 edge 8, and the optimiser obligation of §16.9.6 |
| `sb_store_buffering.xr` | store buffering with `SeqCst` | §16.9.2 edge 8: `SeqCst` operations share one total order |
| `iriw_independent_reads.xr` | independent reads of independent writes | §16.9.2 edge 8: two readers cannot disagree on the order of two `SeqCst` writes |
| `dekker_mutual_exclusion.xr` | Dekker's mutual exclusion | §16.9.2 edge 8, plus edge 12 across worker migration |
| `channel_capacity_edge.xr` | `Channel(N)` used as a semaphore | §16.9.2 edge 2, which nothing else in the suite covers |
