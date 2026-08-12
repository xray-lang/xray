# Typed TargetPlan scalar execution contract

TargetPlan schema 9 may carry a canonical per-function instruction table.
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

Schema 9 is a hard cutover from v8. It preserves the closure-storage,
coroutine-state-call, TargetProfile schema 2 String-literal contract, and
String-literal-storage authorities while adding one sealed, independently
reconstructed `Channel.close()` descriptor. Its receiver is a dispatch target,
not a call argument or authorized frame slot, and it grants no general method
ABI or typed execution path. The String-literal family's
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

The required `COROUTINE_STATE_CALL` family is independent of this
dispatcher. It freezes only the state/resume/direct-call/result-slot relation;
it does not activate coroutine execution or supply child-frame, spill, root,
cleanup, drop, cancel, or action authority.

Evidence:

- `test_typed_dispatch` proves zero-row rejection, wrapping scalar execution,
  production construction and unsupported-function omission,
  SemanticPlan-independent execution, exact XTP roundtrip execution,
  fingerprint/content rejection, prior-schema rejection, and fail-closed mutation of
  opcode, def-use, row identity, function identity, and return structure.
- `test_xtp_format` proves the instruction row width is part of the complete
  exact codec registry.
- `test_typed_frame_runtime_archive` proves the dispatcher and verifier link
  into the runtime-only archive without activating a public execution route.

anchor-sha256: src/plan/target/xr_target_plan.h 541839b09d06c7ff051b4e9b8b648ed2ab27ec66bd85af272d7f0f97e6066989
anchor-sha256: src/plan/target/xr_target_plan.c edec3fad7b0169e2eb683ee4cfe1f4556baec7d372bc1d4f066947c33eb283a4
anchor-sha256: src/plan/target/xr_target_builder.c 31096ca3a4ebc465d1643b72e79e23f221791731bac8a537f9db9bb3048d277d
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 5eea43c77cf0e3802e30eacf12ca7e1a105b7b32de0497635cf7048de1b3438b
anchor-sha256: src/plan/target/xr_target_instruction_verify.c af74c69df7296ff561d3a3abbf4d42a4016e6db54762a750c6da2ad8b4f2ee07
anchor-sha256: src/plan/target/xr_target_verify.c 7cc2d197d6229d767093522c5b9c71483bc4ea70bf3cecb42ccdc2a5f22455e2
anchor-sha256: src/plan/format/xr_xtp_schema.h da104103be67e281c3f6b35658d42022036c3950db492a1d5fd9914e9cc8f9da
anchor-sha256: src/plan/format/xr_xtp_rows.c 4a443bd20e20e63eb9064bb6e81cebb55bcd218abd4a10990b9a689af4a128b8
anchor-sha256: src/plan/format/xr_xtp_encode.c 8cb0983494ace434ec1d1f7389f19d4780ad82f6f88460144e04a9e28c1502bc
anchor-sha256: src/plan/target/xr_xtp_materialize.c b8196ae0744dfa56b23c1a0f93e27aabf761340b0b3ad28c45e952b32b69ebe7
anchor-sha256: src/vm/xr_typed_dispatch.h fd64a88c487eb3ffe90e4fb52aee41bf6b3e5ed78f14bf1106e764d789ae558f
anchor-sha256: src/vm/xr_typed_dispatch.c c4d7a76779f0080211e41147d86721e5dbf90edd3db4395d5e3b18fc0640106a
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 52836fa969629698359a0df893581c3341b45b17977990178dbae12edd438f1c
anchor-sha256: tests/unit/plan/test_xtp_format.c 485e2e33db395bd7020d2a9edc7167a44ee177e6dddde89a92b6558de80ce4a4
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 5a19e39404c8e9decbc72187d1eee8e307bc44484e18422fb705c271ec192b49
