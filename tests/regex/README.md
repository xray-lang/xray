# Regex absolute oracle

This directory owns the backend-independent observable corpus for the regex
capability. It does not own compiler, plan, runtime, build, or completion
schemas.

The versioned corpus has four parts:

- executable programs with byte-exact stdout;
- compile/literal rejection records with exact logical error kinds and offsets;
- hostile RegexPlan mutation records that become executable when the plan
  schema and independent verifier are activated;
- explicit red semantic hazards that must be resolved before activation.

The manifest ratchets the exact case set, source and output digests, negative
and mutation inventory digests, current engine limits and error codes, and the
current Unicode owner digest. The current engine snapshot records its unsafe
one-byte empty-match progress as a red baseline fact. The activation contract
uses Unicode scalar boundaries, emits one final empty match at end-of-input,
then terminates. Match offsets remain UTF-8 byte offsets, so `中😀` yields
offsets `0, 3, 7` and empty replacement yields `-中-😀-`. The executable case
also covers two-, three-, and four-byte scalars for `replace` and `replaceAll`.

`run_absolute_oracle.py` has no case, backend, or skip selector. Every run
requires both VM and AOT to execute every case and produce the frozen output.
The runner also requires a clean matching Release binary from a worktree-local
Ninja build before it executes code. Native provider qualification and builds
use a runner-private probe cache; no other worktree's qualification result is
accepted as evidence.

The current source deliberately has no AOT regex compiler, executor, helper,
or backend rewrite owner. The required VM and AOT matrix is an activation
obligation, not a claim that the current AOT path can run these fixtures. It
must remain non-passing until a verified Xray-backed plan path is activated;
restoring a deleted helper or a runtime-compile fallback cannot satisfy it.

The negative, mutation, and hazard inventories are frozen preparation facts,
not passing test results or product artifact schemas. The Unicode empty-match
replacement hazard records the current invalid-UTF-8 force-unwrap; it cannot be
counted as passing. Activation must preserve valid UTF-8 and return a typed
error for invalid input or result, never panic or split a scalar. These
inventories remain explicit red obligations until their single production
owners are activated. Registering this runner in CTest, changing a protected
schema, or changing a completion baseline is intentionally outside this
directory's ownership.

Validate the corpus without a compiler:

```bash
python3 tests/regex/run_absolute_oracle.py --validate-only
python3 tests/regex/test_absolute_oracle_runner.py
```

Run every executable case against both backends after a matching compiler is
available:

```bash
python3 tests/regex/run_absolute_oracle.py --xray build/xray
```
