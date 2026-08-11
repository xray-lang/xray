# Typed TargetPlan scalar execution contract

TargetPlan schema 7 may carry a canonical per-function instruction table.
Instruction authority is separate from the production AOT family mask: a
verified plan can remain a complete AOT plan while exposing no typed execution
family. A function with zero instruction rows is execution unavailable, never
an empty successful program. The production builder emits a complete group
only for a verified, parameter-free and capture-free, single-return-block
signed-`i64` function whose operations are entirely in the supported family.
Every other function emits zero rows; no partial group or fallback is allowed.

The only supported execution family is a closed straight-line signed `i64`
program consisting of constants, copies, wrapping addition, wrapping
subtraction, wrapping multiplication, and one return. A non-empty function
group must form an exact table partition in canonical function and dense row
order. Its independent verifier requires every referenced slot to belong to
that function and have identical trivial signed-`i64` register and memory
representations. It also proves single assignment, use after definition,
canonical arity and unused fields, and a unique final return. Unknown or
unsupported instructions fail closed.

Instruction rows participate in the TargetPlan fingerprint, the bounded XTP
section directory, the exact 32-byte row codec, and candidate materialization.
The internal scalar dispatcher accepts only an immutable verified plan, its
exact fingerprint, a derived nonzero function execution family, and exact
typed-frame slot identities. It independently recomputes the target-content
fingerprint and does not inspect SemanticPlan or Xi. It has no legacy VM
opcode, `XrValue`, AOT, or generated-C fallback.

Schema 7 is a hard cutover from v6 because exact TargetPlan coverage requires
both closure-storage and coroutine-state-call families. The closure family's
dynamic/owned/tagged row describes
only the outer `XrValue` storage of an exact no-capture heap closure. It grants
no typed instruction, callable-body, allocation, root-map, root-slot, or
cleanup execution authority; the scalar dispatcher continues to reject it.
The C emission projection schema 4 may mechanically spell that verified row
as an exact `TAGGED`/`XrValue` row, but it cannot inspect Xi, Xaot, or source
types to reconstruct one. Its independent verifier requires exact TargetPlan
coverage and rejects missing, extra, reordered, wrong-kind, wrong-spelling,
profile, target, and projection fingerprint mutations.

This is a test-only scalar execution foundation. It adds no public CLI or
installed execution route and does not claim general typed VM instruction
coverage, control flow, calls, aggregates, ownership, exceptions, coroutines,
or complete typed TargetPlan VM execution.

The schema-7 required `COROUTINE_STATE_CALL` family is independent of this
dispatcher. It freezes only the state/resume/direct-call/result-slot relation;
it does not activate coroutine execution or supply child-frame, spill, root,
cleanup, drop, cancel, or action authority.

Evidence:

- `test_typed_dispatch` proves zero-row rejection, wrapping scalar execution,
  production construction and unsupported-function omission,
  SemanticPlan-independent execution, exact XTP roundtrip execution,
  fingerprint/content rejection, v5 rejection, and fail-closed mutation of
  opcode, def-use, row identity, function identity, and return structure.
- `test_xtp_format` proves the instruction row width is part of the complete
  exact codec registry.
- `test_typed_frame_runtime_archive` proves the dispatcher and verifier link
  into the runtime-only archive without activating a public execution route.

anchor-sha256: src/plan/target/xr_target_plan.h 8f8b9ffd674a9d087e2c27389127e8b2b45aa3e41421f2a1f5babfc98a0b7937
anchor-sha256: src/plan/target/xr_target_plan.c 8397740d8365aa0c5f95dd975888e48f74b59b92114b90f2a6463dc8c224db1d
anchor-sha256: src/plan/target/xr_target_builder.c 63ffd7ade14cc5da217f4396fc4b935a29679a9370d02108c462510c2dcc5e00
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 5eea43c77cf0e3802e30eacf12ca7e1a105b7b32de0497635cf7048de1b3438b
anchor-sha256: src/plan/target/xr_target_instruction_verify.c af74c69df7296ff561d3a3abbf4d42a4016e6db54762a750c6da2ad8b4f2ee07
anchor-sha256: src/plan/target/xr_target_verify.c c939aaa937819cb77b72cb647eba5f1f2c96e487c2c194bd88ea15dcdfdaa0fc
anchor-sha256: src/plan/format/xr_xtp_schema.h 32e8d3d61666319edafcd7818161ad9e0adfe952dffd98d9fe65c1e214f69718
anchor-sha256: src/plan/format/xr_xtp_rows.c 86ff377914c671f33304940be3171d3462a76d8b2db6beb368669cc6b6442c5c
anchor-sha256: src/plan/format/xr_xtp_encode.c 8cb0983494ace434ec1d1f7389f19d4780ad82f6f88460144e04a9e28c1502bc
anchor-sha256: src/plan/target/xr_xtp_materialize.c b8196ae0744dfa56b23c1a0f93e27aabf761340b0b3ad28c45e952b32b69ebe7
anchor-sha256: src/vm/xr_typed_dispatch.h fd64a88c487eb3ffe90e4fb52aee41bf6b3e5ed78f14bf1106e764d789ae558f
anchor-sha256: src/vm/xr_typed_dispatch.c c4d7a76779f0080211e41147d86721e5dbf90edd3db4395d5e3b18fc0640106a
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 52836fa969629698359a0df893581c3341b45b17977990178dbae12edd438f1c
anchor-sha256: tests/unit/plan/test_xtp_format.c 27f89010af4d13c3007281b73a0d324ceea7d569760c0b8428c83c6c861d545f
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c f323162dff476e2782c8f522e35d0911cf9c07ba58b050d20890b80cad9b7319
