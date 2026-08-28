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
   operand's separator and the group's terminator from the plan. The group
   remains several instructions because rendering an operand may re-enter the
   interpreter through a user `toString`, and one instruction has nowhere to
   hold a partially rendered group across that call.
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
9. Schema v1 defines no plan flags. Executors render operand by operand, so a
   group is not yet indivisible: a formatter that fails midway has already
   published a prefix, and concurrent groups can interleave within a line.
   Whole-group rendering is a separate capability and will introduce the flag
   that states it; this schema does not promise it in advance.

## Digest anchors

anchor-sha256: src/shared/xr_print_plan.h 51f7910bf13f409520bd76ea2d98515cffa579260b155f31b4175940fc659a88
anchor-sha256: src/shared/xr_core_intrinsic_registry.c 11660c702f0e438919444614f4be66f46cc595220711ec9ebd7e93dc7dc026b0
anchor-sha256: src/ir/xi_lower_expr.c c6aa2113669d90137b367809b9b6fe4b7a588b20f45775abd2dc5ecb85dfe66e
anchor-sha256: src/ir/xi_emit_call.c 698711436b6e940f3d06fcbcbf5a751eab7b59a66c1a0f0cf9f42b3302d42031
anchor-sha256: src/vm/xvm_dispatch_convert.inc.c 6622f7f1d1817ef11f5108d4b3425794a49c3146c5750bbe1e33e9a5fc8e79e2
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 83e8c10fdb3d898578514fba5e9b055530081eeef81b4d98a7304e614f25496e
anchor-sha256: src/plan/semantic/xr_semantic_plan.c 0f78c911fd05636a4717ec9d4d0b8b5db3d8a669a5a680b367960cc8d7923d66
anchor-sha256: src/plan/target/xr_target_capability.h 5069e52a258729e7c1384d1cea8706aba2cfa639d45ad94280bda0d89bd7a3e0
anchor-sha256: src/plan/target/xr_target_builder.c ebe2a51802a1cba0edbcc52dcfd3953cadd4c49268874dcc292c2ead781f6d33
anchor-sha256: src/api/xrepl.c 9c66051d7144b1ab126c76cefd6cb82b000e66887469f99bf3aad47b3ee74912
anchor-sha256: tests/diff/cases/semantics/output/print_zero_args.xr a4c91a7c404a377515c73ed3d75b0548d7d7bb54ae0324c75622376df1ea03b8
anchor-sha256: tests/diff/cases/semantics/output/print_separator_exact.xr 71b81c064808dfd69cc39867555f55c49d3cee23b2c3bfd387f1b5737a825c2f
anchor-sha256: tests/diff/cases/semantics/output/print_terminator_exact.xr 32f13c28e415c770c81bd0eafbfb7d4008b8bdf192f7cb6cade5c01f2845a6a2
anchor-sha256: tests/diff/cases/semantics/output/print_evaluation_order.xr 4ee769724ec8597a3674a631b1f91df82d5c06daa20f2c2c957f648c23a09518
