# Repository agent gates

Changes under `src/ir/`, `src/aot/`, or `src/analysis/` must preserve the Task 218 compiler-memory-safety defenses:

- Run `ctest --test-dir build -R meta_ownership_inventory` after compiler metadata changes.
- Run `ctest --test-dir build -R asan_focused` before handing off a completed change in those directories.
- Strings and metadata crossing AST, analyzer, IR, plan, evidence, or CGen stage boundaries must be copied into the receiving arena/pool or transferred explicitly. Do not retain dynamic-array element pointers across operations that can grow the array.
- The generated-C W1-W4 verifier is always on. Do not add a bypass, downgrade its ICE behavior, or hand malformed output to the host C compiler.

Document any platform-only LeakSanitizer suppression in `scripts/lsan.supp`; the suppression budget may shrink but may not grow without an explicit contract review.
