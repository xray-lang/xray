# Repository agent gates

Changes under `src/ir/`, `src/aot/`, or `src/analysis/` must preserve the Task 218 compiler-memory-safety defenses:

- Run `ctest --test-dir build -R meta_ownership_inventory` after compiler metadata changes.
- Run `ctest --test-dir build -R asan_focused` before handing off a completed change in those directories.
- Strings and metadata crossing AST, analyzer, IR, plan, evidence, or CGen stage boundaries must be copied into the receiving arena/pool or transferred explicitly. Do not retain dynamic-array element pointers across operations that can grow the array.
- The generated-C W1-W4 verifier is always on. Do not add a bypass, downgrade its ICE behavior, or hand malformed output to the host C compiler.

Document any platform-only LeakSanitizer suppression in `scripts/lsan.supp`; the suppression budget may shrink but may not grow without an explicit contract review.

## Semantic contract freeze

The machine-checked semantic contracts live in `contracts/`. Before changing a
listed anchor, read the owning contract and decide whether existing diff cases,
KATs, shape gates, or ports evidence must be migrated.

- Refresh every affected `anchor-sha256` record in the same commit.
- Add `CONTRACT-CHANGE: contracts/<file>.md <one-line reason>` to the commit
  message for each affected contract.
- Record how affected evidence was rerun, regenerated, or retired. Retirement
  must use the governed tombstone inventory; never silently delete evidence.
- Run `ctest --test-dir build --output-on-failure -R contract_freeze` after the
  commit so trailer enforcement runs against the final commit message.
