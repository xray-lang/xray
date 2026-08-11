# Typed TargetPlan scalar execution contract

TargetPlan schema 8 may carry a canonical per-function instruction table.
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

Schema 8 is a hard cutover from v7 because exact TargetPlan coverage preserves
the closure-storage and coroutine-state-call families and additionally requires
TargetProfile schema 2's structured runtime String-literal contract and the
dedicated String-literal-storage family. That family's
dynamic/owned/tagged row describes only an exact frozen String literal's outer
value and grants no object allocation, root-map, cleanup, tuple, or general
owned-String authority. The closure-storage family's dynamic/owned/tagged row describes
only the outer `XrValue` storage of an exact no-capture heap closure. It grants
no typed instruction, callable-body, allocation, root-map, root-slot, or
cleanup execution authority; the scalar dispatcher continues to reject it.
The C emission projection schema 5 mechanically spells both verified dynamic
families as exact `TAGGED`/`XrValue` rows. For an exact String literal it also
owns the immutable literal bytes and the explicit String-view materialization
recipe. CGen consumes that recipe mechanically and has no Xi/type fallback.
The independent verifier reconstructs expected literal bytes only from the
frozen SemanticPlan bound through TargetPlan and rejects missing, extra,
reordered, wrong-kind, wrong-spelling, wrong-recipe, profile, target, and
projection fingerprint mutations.

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
  fingerprint/content rejection, v6 rejection, and fail-closed mutation of
  opcode, def-use, row identity, function identity, and return structure.
- `test_xtp_format` proves the instruction row width is part of the complete
  exact codec registry.
- `test_typed_frame_runtime_archive` proves the dispatcher and verifier link
  into the runtime-only archive without activating a public execution route.

anchor-sha256: src/plan/target/xr_target_plan.h 10adcaa8f644a39abf670d5367b611758524afe0b313a0d484a824fa7360505d
anchor-sha256: src/plan/target/xr_target_plan.c e0b3299388c4b0f79c69ef761e9e06785bb0182fcf6b62573a0b4a4cc31e3368
anchor-sha256: src/plan/target/xr_target_builder.c e42a088e616196e4d14546b8e4992057857f00fb8b78228d0c76d8a3187d88b5
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 5eea43c77cf0e3802e30eacf12ca7e1a105b7b32de0497635cf7048de1b3438b
anchor-sha256: src/plan/target/xr_target_instruction_verify.c af74c69df7296ff561d3a3abbf4d42a4016e6db54762a750c6da2ad8b4f2ee07
anchor-sha256: src/plan/target/xr_target_verify.c e2b50c87c7c2b5f8415a25646f3947293761c49b9fad8a9206fb559e1124744e
anchor-sha256: src/plan/format/xr_xtp_schema.h dc1d1ee39d90fa7b00f49c8efa7484df5311524d8abaeda2f3ac7c4d61a8bb5f
anchor-sha256: src/plan/format/xr_xtp_rows.c 4a443bd20e20e63eb9064bb6e81cebb55bcd218abd4a10990b9a689af4a128b8
anchor-sha256: src/plan/format/xr_xtp_encode.c 8cb0983494ace434ec1d1f7389f19d4780ad82f6f88460144e04a9e28c1502bc
anchor-sha256: src/plan/target/xr_xtp_materialize.c b8196ae0744dfa56b23c1a0f93e27aabf761340b0b3ad28c45e952b32b69ebe7
anchor-sha256: src/vm/xr_typed_dispatch.h fd64a88c487eb3ffe90e4fb52aee41bf6b3e5ed78f14bf1106e764d789ae558f
anchor-sha256: src/vm/xr_typed_dispatch.c c4d7a76779f0080211e41147d86721e5dbf90edd3db4395d5e3b18fc0640106a
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 52836fa969629698359a0df893581c3341b45b17977990178dbae12edd438f1c
anchor-sha256: tests/unit/plan/test_xtp_format.c 3d9bc6a45ec4749720cc5457e71f624497d8070eba27406aa866a215fe604572
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 1df1c5d8130a740d9737e735477467501bb0e095289f061bc886ae9dc6dd9dc9
