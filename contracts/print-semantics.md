# Print Semantics Contract

This contract freezes the typed source, semantic, Xi, VM, AOT, and target
capability boundary for grouped output.

1. `print` is an ordinary call with one compiler-owned source identity. Binding
   and lowering select it through the stable registry row, never through a
   source-name comparison, and no statement-specific AST node exists.
2. One source call is one plan. The separator between operands and the
   terminator after the last one are facts of the group; no operand carries a
   copy of a decision the group already made. Arity zero is a complete group
   and still writes its terminator.
3. Operands evaluate left to right, exactly once each, before any of them is
   rendered as text.
4. Bytecode and generated C are projections of the group. Emission derives each
   operand's separator and the group's terminator from the plan. A group
   renders into one buffer and reaches the output capability exactly once, at
   the end; every operand instruction appends to that buffer and can no more
   write than it can choose a separator.
   The buffer is an ordinary value holding a register, so it survives a user
   `toString` re-entering the interpreter by living in the frame the call
   returns to, and a nested group gets a buffer of its own. Several
   instructions is what makes that re-entry a suspendable call: collapsing the
   group into one instruction would require driving `toString` from C, and
   `xr_vm_call_closure` reports a suspension as a failure, so `await` inside a
   `toString` would stop working. The shape is a requirement, not a
   concession.
5. A full-width unsigned operand carries an explicit marker because its slot
   representation is indistinguishable from a signed one. Narrower unsigned
   types are non-negative in that slot and carry none.
6. Rendering a unit-typed operand as `()` is decided by the static type, so no
   renderer distinguishes unit from null at run time.
7. Target planning derives the output capability from the frozen target
   profile. Hosted output uses the process stream the executor already owns.
   Freestanding output requires an exact output-write capability, which is a
   distinct identity: an assertion-report provider must not stand in for
   ordinary program output. A missing capability fails before C emission.
8. REPL auto-display is not a second output semantics. It elaborates a guarded
   ordinary call, so no suppress-null flag travels through the plan, the
   bytecode, or any backend.
9. `ATOMIC_GROUP` states that a group is indivisible. The buffer has exactly
   one exit: the flushing instruction. Dying any other way discards it
   unwritten — a panic unwinding out of a formatter, a coroutine collected
   while suspended inside one, or any other release. These are not three
   rules; they are one rule seen from three directions, and the rule is that
   nothing but the flush publishes anything. A formatter that fails partway
   through therefore leaves no prefix behind, not even the separator that
   would have preceded the operand that failed.
   What an escaped prefix costs is more than the prefix: it carries no
   terminator, so the next group continues the line it was stranded on, and
   one failed call goes on corrupting the framing of everything printed after
   it.
10. Indivisibility of the flush itself is a separate fact from the count of
    instructions that can reach it, and the two backends get it from different
    places. Hosted output is a single stdio call on the stream the executor
    owns, and stdio locks the `FILE` object, so a concurrent group cannot land
    inside it even when the runtime is running coroutines on several threads;
    writing through `write(2)` or an `_unlocked` variant would break this
    without changing any instruction count. Freestanding output is a single
    call to the exact output-write provider operation, and its indivisibility
    is the provider's obligation, stated in the capability contract.
    So verifying that one group reaches the output capability once is
    sufficient for hosted output; for freestanding output it establishes that
    no partial group is published, while non-interleaving rests on the
    provider.

## Digest anchors

anchor-sha256: src/shared/xr_print_plan.h 3bf58c71e0872cd80f761f40cf772086181274a8742da5cc0401d97f868facee
anchor-sha256: src/shared/xr_core_intrinsic_registry.c 97197195363f6f21f9eefaa6440373e369ab70569a02aa4fe4afba8cac3ce13c
anchor-sha256: src/ir/xi_lower_expr.c 255f4e9d373d03f25a13afe05ce2ec69e0d2da60452e61b4a5d547a464cd63ab
anchor-sha256: src/ir/xi_emit_call.c f99706984af44243a0aa97651795f78f4247cbc45af1761ef4602f3aeb2180f8
anchor-sha256: src/vm/xvm_dispatch_convert.inc.c b7b703389d77714d167f64f4f8f4fbe8e934da4428324e4a590e485e86aabb2a
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 706460461cfc548a14773f320bc4764f034b5446a810246ee00d9a562061f379
anchor-sha256: src/plan/semantic/xr_semantic_plan.c 0f78c911fd05636a4717ec9d4d0b8b5db3d8a669a5a680b367960cc8d7923d66
anchor-sha256: src/plan/target/xr_target_capability.h 5069e52a258729e7c1384d1cea8706aba2cfa639d45ad94280bda0d89bd7a3e0
anchor-sha256: src/plan/target/xr_target_builder.c d27e05bc59d35c7f119648d308d4f9750d7c785845dd63cb217278279e9fcfa1
anchor-sha256: src/api/xrepl.c 9c66051d7144b1ab126c76cefd6cb82b000e66887469f99bf3aad47b3ee74912
anchor-sha256: tests/diff/cases/semantics/output/print_zero_args.xr a4c91a7c404a377515c73ed3d75b0548d7d7bb54ae0324c75622376df1ea03b8
anchor-sha256: tests/diff/cases/semantics/output/print_separator_exact.xr 71b81c064808dfd69cc39867555f55c49d3cee23b2c3bfd387f1b5737a825c2f
anchor-sha256: tests/diff/cases/semantics/output/print_terminator_exact.xr 32f13c28e415c770c81bd0eafbfb7d4008b8bdf192f7cb6cade5c01f2845a6a2
anchor-sha256: tests/diff/cases/semantics/output/print_evaluation_order.xr 4ee769724ec8597a3674a631b1f91df82d5c06daa20f2c2c957f648c23a09518
anchor-sha256: tests/diff/cases/semantics/output/print_group_atomic_on_panic.xr b089ad68db85aabc4aa18e338242096a56dbf91b51eaecc9c75e5cfaa4db622c
anchor-sha256: tests/diff/cases/semantics/output/print_group_atomic_on_fatal.xr 491edb1d65f573bd0237ea4bd4d1f4d3f3609dae317509113345c3aa5b343746
anchor-sha256: tests/diff/cases/semantics/output/print_unsigned_widths.xr 577699990edb47f23009271d683ac25fc46f93ff3b2eaa9db525904714f0e1d0
