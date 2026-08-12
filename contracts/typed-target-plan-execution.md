# Typed TargetPlan scalar execution contract

TargetPlan schema 10 may carry a canonical per-function instruction table.
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

Schema 10 is a hard cutover from v9. It preserves the closure-storage,
coroutine-state-call, TargetProfile schema 2 String-literal contract, and
String-literal-storage authorities and the sealed, independently reconstructed
`Channel.close()` descriptor while adding direct-local-callee storage. Its receiver is a dispatch target,
not a call argument or authorized frame slot, and it grants no general method
ABI or typed execution path. The String-literal family's
dynamic/owned/tagged row describes only an exact frozen String literal's outer
value and grants no object allocation, root-map, cleanup, tuple, or general
owned-String authority. The closure-storage family's dynamic/owned/tagged row describes
only the outer `XrValue` storage of an exact no-capture heap closure. It grants
no typed instruction, callable-body, allocation, root-map, root-slot, or
cleanup execution authority; the scalar dispatcher continues to reject it.
The direct-local-callee-storage row is a borrowed dynamic outer `XrValue`
token. Independent builder, Target verifier, and AOT materialization verifier
prove that every use is operand zero of the same frozen direct-local call and
that the live shared slot names the unique canonical child. It grants no
closure allocation, body, root, cleanup, or indirect-call authority.
The C emission projection schema 6 mechanically spells all verified dynamic
families as exact `TAGGED`/`XrValue` rows. For an exact String literal it also
owns the immutable literal bytes and the explicit String-view materialization
recipe. CGen consumes that recipe mechanically and has no Xi/type fallback.
The independent verifier reconstructs expected literal bytes only from the
frozen SemanticPlan bound through TargetPlan and rejects missing, extra,
reordered, wrong-kind, wrong-spelling, wrong-recipe, profile, target, and
projection fingerprint mutations.

The runtime generation authority exposes this family through one public
product route: exact XSM bytes construct runtime-owned semantic and native
profile authority, which may load their matching XTP into a sole-function
scalar-i64 generation and execute function 0. PREPARE requires exactly one
canonical function, this exact nonempty execution family, the typed-frame
schema/family closure, and no storage, allocation, call, root, cleanup,
adapter, or coroutine execution authority. Execution requires healthy ACTIVE
state and a balanced in-flight-call pin. This adds no public CLI, export
selection, or general typed VM instruction coverage, control flow, calls,
aggregates, ownership, exceptions, coroutines, or complete typed TargetPlan
VM execution.

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
  exact codec registry and exercises the public XSM/XTP generation route.
- `test_typed_frame_runtime_archive` proves the dispatcher and verifier link
  into the runtime-only archive; the runtime artifact archive gate separately
  proves the exact XSM/XTP sole-function product route.
- `test_runtime_generation` proves exact sole-function PREPARE, ACTIVE scalar
  execution, unsupported-plan rejection, bounded pins, drain, retirement, and
  unload without any legacy execution fallback.

anchor-sha256: src/plan/target/xr_target_plan.h fcb5361be828a1a0164c9ba6d01f0df7c9444f6f5088b59025158a8f2c66ad3d
anchor-sha256: src/plan/target/xr_target_plan.c b6bf2d627ceac8ea3b4878bfa543ac8f131e323c916af4bd7edce5bec28537f2
anchor-sha256: src/plan/target/xr_target_builder.c 532dde8cee908bee915dc137b55cafe93a4b910f9f007c2994772c107fe3a1c8
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 5eea43c77cf0e3802e30eacf12ca7e1a105b7b32de0497635cf7048de1b3438b
anchor-sha256: src/plan/target/xr_target_instruction_verify.c af74c69df7296ff561d3a3abbf4d42a4016e6db54762a750c6da2ad8b4f2ee07
anchor-sha256: src/plan/target/xr_target_verify.c 2997198df1c8e8886f99674e04c677d0e37901fcc755a9bf2e289cc9b0c832e0
anchor-sha256: src/plan/format/xr_xtp_schema.h 257b840e064875dde48ace6dd593b9ae98f533df0994983996d0d11a1ee13f72
anchor-sha256: src/plan/format/xr_xtp_rows.c 4a443bd20e20e63eb9064bb6e81cebb55bcd218abd4a10990b9a689af4a128b8
anchor-sha256: src/plan/format/xr_xtp_encode.c 8cb0983494ace434ec1d1f7389f19d4780ad82f6f88460144e04a9e28c1502bc
anchor-sha256: src/plan/target/xr_xtp_materialize.c b8196ae0744dfa56b23c1a0f93e27aabf761340b0b3ad28c45e952b32b69ebe7
anchor-sha256: src/vm/xr_typed_dispatch.h f72964091ac427130a3ff00c6d051cf85a3edd6ae174846984e0c1d506adeecd
anchor-sha256: src/vm/xr_typed_dispatch.c 3fd358dc6b4aaa5ce0ff2f039a1b62e0420bede465473c8cfc2da51980e94945
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 52836fa969629698359a0df893581c3341b45b17977990178dbae12edd438f1c
anchor-sha256: tests/unit/plan/test_xtp_format.c f97746e16b612fa047054ff26f3d1ba0a47597b48c04cf061ebf1233d78a2de4
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c a34cb68cf7ca7832312737dbd485791297766d9f300a3532669c4df55e6a74cd
anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation.c d02e74f29b281d354ea03b339205e457e62266c06bd4b1afb6692d90a2c0e1d7
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 993a338ba5dd2f0ed7a88f4aa830e697700361d88acd2b6ef36f35bcafc270a7
