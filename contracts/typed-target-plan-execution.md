# Typed TargetPlan scalar execution contract

TargetPlan schema 16 may carry a canonical per-function instruction table.
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

Schema 16 is a hard cutover from v15 and all earlier schemas. It preserves all
v15 authorities while adding a sealed `StringBuilder()` constructor call whose
identity binds the exact Semantic allocation ID and whose result is an owned
dynamic slot. It grants no generic builtin call authority. Its
`Channel.close()` receiver is a dispatch target,
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
that the live shared slot names the unique canonical child of the first lexical
slot owner. A caller-local store must dominate the load; the only parent-scope
exception is a unique module-root entry-prefix initializer before any
activation-shaped operation. Thus a root-owned sibling helper is exact without
turning arbitrary shared values into call authority. It grants no closure
allocation, body, root, cleanup, or indirect-call authority.
The direct-local-GO-callee-storage row is a distinct borrowed dynamic outer
`XrValue` token. Three independent reconstructions prove one canonical
closure initializer, shared-slot dominance, unique canonical local child and
signature, and that every use is operand zero of `XI_GO` for that same child.
It grants no GO task-result, child-task object, closure allocation/body, root,
cleanup, argument storage, or coroutine execution authority.
The Channel-allocation-storage family binds an exact frozen `XI_CHAN_NEW`
result to owned dynamic outer `XrValue` storage and exact identity-copy aliases
to borrowed dynamic outer storage. Independent builder, Target verifier, and
AOT materialization verifier separately reconstruct the canonical allocation
key/id, Channel element and capacity types, ownership/provenance, and copy
shape. This family supplies no Channel object-body layout, allocation
execution, root map, root slot, cleanup, transfer-plan, tuple, or general
object authority.
The Channel-receive-storage family binds only an exact frozen
`XI_CHAN_TRY_RECV` scalar result to trivial native storage after independently
proving that its receiver belongs to the Channel-allocation identity chain and
that `Channel<T>`'s sole child is the result type. It grants no receive
scheduling, Channel body, ownership-transfer, aggregate, tuple, root, or
cleanup authority.
Schema 16 also preserves SOURCE_EXPORT dispatch authority only when the caller's
verified SemanticPlan is accompanied by its exact ordered verified dependency
plan vector. The Target call row binds dependency ordinal, public export ID,
and dependency callee stable ID, while keeping the target-local callee index
absent. The matching coroutine row binds the same call and result slot. This
is public-wrapper identity and suspension authority only: it supplies no
cross-module argument slots, private/native callee, child frame, roots,
cleanup, drop, cancel, or executable call ABI. Standalone materialization and
all ungrounded method calls remain fail closed.
The dedicated SOURCE-namespace-storage rows cover only the borrowed dynamic
outer `XrValue` tokens in the exact
`IMPORT_REF -> identity COPY* -> SET_SHARED -> GET_SHARED -> identity COPY*`
chain used as SOURCE_EXPORT call receivers. Identity-COPY chains are bounded,
acyclic, and same-function; every endpoint and COPY keeps its own exact slot
identity and unique expected consumer. Three independent reconstructions prove
dependency/module identity, unique shared slot, complete use sets, and receiver
binding. They grant no imported module object body, allocation,
root, cleanup, member lookup, dependency activation, argument ABI, or
cross-module frame.
The C emission projection schema 10 mechanically spells all verified dynamic
families as exact `TAGGED`/`XrValue` rows. For an exact String literal it also
owns the immutable literal bytes and the explicit String-view materialization
recipe. For exact `XI_CHAN_NEW` it owns the helper spelling and capacity
semantic-value recipe. For exact `XI_CHAN_TRY_RECV` it owns the receiver
semantic value and exact scalar unbox helper spelling. Sync and coroutine CGen
consume those recipes mechanically and have no Xi/type or legacy fallback.
The independent verifier reconstructs expected literal bytes only from the
frozen SemanticPlan bound through TargetPlan and rejects missing, extra,
reordered, wrong-kind, wrong-spelling, wrong-operand, wrong-recipe, profile, target, and
projection fingerprint mutations.
For the sealed `StringBuilder()` call it owns the zero-operand
`xrt_strbuf_new` materialization recipe. Sync and coroutine CGen have no
name-based fallback for that constructor.

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

anchor-sha256: src/plan/target/xr_target_plan.h 4640ef8d38961ef96f29ac6a26e0e6469182d327d2501301b51feeb4bef44dec
anchor-sha256: src/plan/target/xr_target_plan.c e2898a5a6773f501917767150df2f7aacef48de683c6ce5e12cecd8fdf90f8ca
anchor-sha256: src/plan/target/xr_target_builder.c 9076f181d58013a5595bccfea215d1e494ba312e43d85704cd86b6de625c8111
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 5eea43c77cf0e3802e30eacf12ca7e1a105b7b32de0497635cf7048de1b3438b
anchor-sha256: src/plan/target/xr_target_instruction_verify.c af74c69df7296ff561d3a3abbf4d42a4016e6db54762a750c6da2ad8b4f2ee07
anchor-sha256: src/plan/target/xr_target_verify.c a2a8384d64ff412705066fffd31b4428299c01fd75895d4a6b29c3b03f84c2d8
anchor-sha256: src/plan/format/xr_xtp_schema.h 1267b8734e2780e0ced0e2ef931c09aec8ebaaaeb966accc2c79a83531bea678
anchor-sha256: src/plan/format/xr_xtp_rows.c 85e8842a3857fd250c68c5cc12b7aba35787461650317dacdb39eaf92da317a9
anchor-sha256: src/plan/format/xr_xtp_encode.c 8cb0983494ace434ec1d1f7389f19d4780ad82f6f88460144e04a9e28c1502bc
anchor-sha256: src/plan/target/xr_xtp_materialize.c c43b819e15e02784e50911b46ad3ec228b2069c114a7691216abbf59ab6bcdbf
anchor-sha256: src/vm/xr_typed_dispatch.h f72964091ac427130a3ff00c6d051cf85a3edd6ae174846984e0c1d506adeecd
anchor-sha256: src/vm/xr_typed_dispatch.c 3fd358dc6b4aaa5ce0ff2f039a1b62e0420bede465473c8cfc2da51980e94945
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 52836fa969629698359a0df893581c3341b45b17977990178dbae12edd438f1c
anchor-sha256: tests/unit/plan/test_xtp_format.c 5f69974f555b1d9601471299fdc3dbc109af50a919ab89ab65c9e8a787db0cf9
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c fd1d0a25a4a0f634fde4ceb37f64bffc43a65da91ed5d725b7bb09e1d327df25
anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation.c d02e74f29b281d354ea03b339205e457e62266c06bd4b1afb6692d90a2c0e1d7
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 993a338ba5dd2f0ed7a88f4aa830e697700361d88acd2b6ef36f35bcafc270a7