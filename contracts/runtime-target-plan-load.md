# Runtime TargetPlan artifact load contract

This contract freezes exact XSM authority construction and the TargetPlan load
boundary. Loading itself remains non-executing; the separately governed
generation lifecycle may execute only the sole-function scalar route. This
contract does not claim provider registration, an entry cell, exports, calls,
roots, or general module activation.

1. Artifact identity comes from bytes. The probe reports match, need-more,
   unknown-reserved, or extension conflict. Only exact XSM and XTP identities
   are recognized compiled artifacts. Every retired `XRAY...` container prefix
   is an unknown reserved identity and fails before runtime creation; the
   public classifier carries no legacy XRC kind, version, reader, or extension
   alias. A file name can only confirm XSM/XTP identity or conflict with it; it
   cannot create one.
2. XTP candidate decoding copies caller bytes into owned immutable storage
   before parsing any header, directory, digest, or typed row. Candidate
   materialization reads only that snapshot. The shared hard peak covers the
   caller read buffer, candidate snapshot, decoded tables, and the frozen-plan
   copy with checked arithmetic. The instruction section is a bounded compact
   stream: its wire-byte length, expanded canonical-row count, and decode work
   are independently charged before materialization.
   A standalone stress gate grows valid CFG, row, table-byte, and artifact-byte
   workloads, records decode/materialize wall time and process peak memory,
   and rejects each hard manifest or section-row budget at one past its limit
   with a repeatable diagnostic. It also mixes fresh decode, shared-candidate
   materialization, and generation pins across threads. Root and cleanup rows
   remain unavailable to valid artifacts, and there is no standalone debug
   section; the gate records that boundary instead of manufacturing rows that
   the independent verifier cannot authorize.
3. A runtime artifact authority retains an independently verified
   SemanticPlan and a runtime-constructed exact TargetProfile. Its internal
   factory has no TargetProfile input: the native runtime owns and rebuilds
   the canonical architecture, operating system, environment, native ABI,
   complete data layout, atomic widths/orders, float features, vector
   features/width, runtime profile, ABI, object header, structured String
   literal view materialization, and provider set before building the profile.
   TargetProfile schema 2 carries the verified String-literal field layout,
   dynamic tag, ownership/termination policy, and materialization recipe; a
   caller-supplied raw digest is not authority. The independent verifier
   re-derives the same fields; caller-authored profile or runtime/provider/
   object fingerprints cannot authorize a foreign machine or a foreign XTP
   artifact.
   The current runtime has no governed SIMD discovery or installed SIMD
   manifest, so its exact native execution authority is scalar-only. Any
   nonzero vector feature or maximum vector width fails closed.
4. The required TargetPlan family mask is exactly scalar, aggregate,
   direct-local call, closure storage, minimal coroutine state-call,
   String-literal storage, direct-local-callee storage, Channel-allocation
   storage, Channel-receive storage, direct-local-GO-callee storage,
   direct-local-GO-task-result storage, panic-catch storage,
   SOURCE-import storage, and ADT-enum storage. The
   scalar family admits an unaliased SemanticPlan `Ptr` or `MutPtr` only as an
   exact TargetPlan `RAW_PTR` with target-profile pointer layout and a trivial,
   non-root, null-zero lifecycle. The independent verifier re-parses the frozen
   type identity and rejects incomplete or mutated representation facts; no Xi
   type, name, or legacy plan can authorize it. The
   closure-storage family covers
   only an exact no-capture heap closure's outer `XrValue` slot as
   dynamic/owned/tagged storage. It does not authorize the closure object body,
   allocation, root map, root slot, cleanup, or executable callable body.
   The coroutine table proves only the frozen state, resume predecessor,
   direct call, and result-slot relation. It grants no child-frame, spill,
   root, cleanup, drop, cancel, or execution authority.
   The String-literal family similarly covers only the dynamic/owned/tagged
   outer value of an exact frozen `XI_CONST` String with static-borrow return
   provenance. It authorizes no general owned String object, tuple, allocation,
   root map, root slot, or cleanup path.
   Direct-local-callee storage covers only a borrowed dynamic outer `XrValue`
   loaded from one shared slot when every use is the callee of the same frozen
   direct-local call target and the frozen target is the unique child of the
   first lexical shared-slot owner. Caller-local stores require dominance; the
   sole parent-scope exception is a canonical module-root entry-prefix store
   before activation. It grants no closure allocation/body, root map, cleanup,
   general indirect call, or executable callable authority.
   Direct-local-GO-callee storage is a separate family for the same outer
   borrowed dynamic callable token when it is produced by one canonical
   entry initializer (`CLOSURE_NEW` then `SET_SHARED`) and every use of the
   matching `GET_SHARED` is operand zero of `XI_GO` for the same canonical
   local child. A shared slot belongs to one module-wide table, so the
   initializer is named by the slot index alone and must sit in the scope
   that lexically owns the slot, which is the frozen child's own parent. A
   store the loading function makes itself must dominate the load; a store
   in an enclosing owner instead has to be that scope's entry-prefix
   initializer, before any activation-shaped operation. A module-level
   helper started from another function is therefore exact, while a store
   in an unrelated function is not authority for any load.
   Builder, Target verifier, and AOT materialization verifier
   independently reconstruct that owner and initializer relation, the
   unique child, exact signature, slot, ownership/provenance, and complete
   use set. It grants no
   GO task-result, child-task object, callable body, allocation, root map,
   root slot, cleanup, argument storage, or coroutine execution authority.
   Direct-local-GO-task-result storage separately binds the exact `Task<T>`
   result of that proved GO to a borrowed dynamic/tagged temporary. The
   runtime executor owns the task object; builder, independent Target verifier,
   and AOT representation refinement all re-prove the Task nominal identity,
   the exact callee operand, the GO result, and the `AWAIT` carrier use. This
   family grants no task body, allocation, root slot, ARC cleanup, scheduling,
   or general coroutine execution authority.
   A native-namespace yieldable call row is likewise derived only from the
   frozen `NATIVE_NAMESPACE_YIELDABLE` SemanticPlan target. It carries the
   suspension convention and result slot but no backend symbol, source
   dependency, callee function, or argument ABI; missing registry-backed
   identity remains unavailable rather than falling back to selector text.
   A builtin-instance yieldable call row is derived only from the frozen
   `BUILTIN_INSTANCE_YIELDABLE` SemanticPlan target. The receiver's frozen
   builtin identity and the selector's argument count select one entry of the
   bounded suspending-method roster, so the row compares no spelling and a user
   class reusing a builtin name carries builtin identity zero and matches
   nothing. Builder and independent verifier share the one judgement rather
   than restating it. The row carries the suspension convention and names the
   receiver type; it grants no backend symbol, callee function, provider
   spelling, frame layout, argument ABI, or result materialization, and the
   call stays unavailable until a storage family binds that result.
   Channel-allocation storage covers the owned dynamic outer `XrValue` result
   of an exact frozen `XI_CHAN_NEW` and borrowed dynamic identity-copy aliases.
   It independently binds canonical allocation key/id, exact Channel/capacity
   types, and ownership/provenance, but grants no Channel object-body layout,
   allocation execution, root map, root slot, cleanup, transfer plan, tuple,
   or general object authority.
   Channel-receive storage covers only the trivial scalar outer slot produced
   by an exact frozen `XI_CHAN_TRY_RECV`. Independent builder and verifier
   reconstruction require an exact Channel-allocation identity chain and prove
   that `Channel<T>`'s sole frozen child is the result type. The row does not
   authorize Channel object layout, receive scheduling, ownership transfer,
   aggregate payloads, tuple payloads, roots, or cleanup.
   Direct-local unit-enum argument storage covers only source-backed, payload-
   free enum ordinals whose declaration identity and nominal layout are frozen
   in SemanticPlan. The target row is a trivial signed 64-bit ordinal bound to
   that exact semantic type and direct-local parameter/argument relation. It
   grants no payload enum, boxing, allocation, root, cleanup, or dispatch
   authority. That ordinal is this family's to bind: the scalar family's
   result-void shortcut reads the generated opcode table alone, and it stops
   at a unit-enum type rather than publishing a second, contradictory void
   storage fact for the same value.
   SOURCE-import storage covers only borrowed dynamic outer `XrValue` tokens
   in either the exact namespace
   `IMPORT_REF -> identity COPY* -> SET_SHARED -> GET_SHARED -> identity COPY*`
   receiver chain or the exact named-export
   `IMPORT_REF(member) -> SET_SHARED -> GET_SHARED` callee chain consumed by
   the same SOURCE_EXPORT call. Identity-COPY chains are
   bounded, acyclic, and same-function; every endpoint and COPY keeps its own
   exact slot identity and unique expected consumer. Builder, Target verifier,
   and AOT materialization verifier independently reconstruct the ordered
   dependency, module identity, shared slot, complete use sets, and call binding.
   These rows grant no imported module object body, allocation, root, cleanup,
   guessed member lookup, dependency activation, or cross-module frame. A
   parameterized SOURCE_EXPORT separately carries dense call-argument rows
   binding dependency parameter stable identity, caller operand/value/slot,
   mode, ownership, transfer, and identical machine representation. An exact
   `ref` row authorizes one additional C pointer level; Xi/name/type and legacy
   representation plans cannot create that authority.
   Source-class-object storage covers only the owned dynamic outer `XrValue`
   produced by an exact frozen `XI_CLASS_CREATE`. The allocation's result type
   is the erased reference the IR selects for it, so the class identity is
   proved instead from the plan's own source-class table matched by the
   operation's own class name; a name matching two declarations or none names
   nothing and is refused. Every class allocation in the module must be named,
   and no two may claim one declaration. Builder, Target verifier, and AOT
   representation oracle re-derive the same judgement independently. The row
   grants no class object body, field table, method table, allocation
   execution, root map, root slot, cleanup, construction, or member lookup
   authority.
   Source-class-instance storage covers the four values an instance can be:
   the borrowed read of the class object a construction dispatches on, the
   owned instance a construction returns, the owned instance a direct-local
   call returns, and the borrowed reads of an instance out of its own module
   slot. All four are outer tagged dynamic values for the same reason the class
   object is, and ownership is never a property of the family: it is the
   operation's own result ownership, owned for either kind of call and borrowed
   for either read, so a read can never be frozen as an owning root. A returned
   instance is a transfer of the outer tagged value exactly as an owned String
   or Array result is: the return must be fresh and whole rather than aliased
   or forwarded through a return parameter, and the callee's declared return
   contract must state what the call site reads. A read is proved through the
   one value its slot holds, and that value is an instance for either reason an
   instance exists, so how the allocation arrived never decides whether the
   read can be named. The construction itself carries a call dispatch kind driven
   by the SemanticPlan call target of the same name; it names no callee
   function and never makes its caller suspendable, because the shared
   judgement admits only a call whose effects are the generated call effects.
   What it may pass is not the call's word to give: the arity and the contract
   are read off the declaration's single recorded constructor, whose parameters
   after the receiver must match the operands one for one and in order, with
   argument ordinal zero binding parameter ordinal one because the construction
   supplies the receiver rather than passing it. Only a parameter carrying no
   owning reference is admitted. One flag is deliberately not required to match
   the generated default: the annotation an optimizer writes on an all-constant
   call states nothing about effects, ownership or contract. Builder, Target
   verifier, and AOT representation oracle re-derive both the storage and the
   dispatch through that one judgement. These rows grant no class body, field
   table, method table, root map, root slot, cleanup, or member lookup
   authority.
   Source-class-receiver storage covers the instance a constructor is entered
   with. It is the one value of the construction family whose own type row
   cannot name what it is, because the frontend types the receiver as a bare
   instance naming no declaration; its declaration is read off the function
   instead, as the parameter that the parameter range of a function recorded as
   the constructor of one frozen declaration starts with. It is an outer tagged
   dynamic value for the same reason the class object is, it carries a
   parameter slot rather than a temporary because it is bound on entry rather
   than computed, and its ownership is the parameter's own recorded ownership
   rather than a property of the family. A field write through it is admitted
   only for a stored value that has a machine storage row of its own, so a
   field whose type has none is refused rather than given a tagged guess.
   Builder, Target verifier, and AOT representation oracle re-derive the same
   judgement independently. The row grants no class body, field table, method
   table, allocation, root map, root slot, cleanup, or member lookup authority.
   ADT-enum storage covers only exact source-backed enum parameters, direct
   local returns, and payload-bearing constructor results whose declaration,
   member, nominal layout, discriminant, ordered payload types, namespace
   receiver, and ownership are frozen in SemanticPlan. Constructor payloads
   are a CEmission recipe rather than Target call arguments. Direct-local enum
   arguments carry both exact caller and callee representations so ownership
   may select distinct physical rows without weakening the shared tagged ABI.
   This family grants no guessed enum name, mutable Xi type authority, generic
   method dispatch, object body, root map, cleanup, or fallback boxing path.
   Foundation capability masks are also exact. Allocator and panic
   requirements have dense records bound to the same canonical provider kinds.
   Missing, duplicate, additional, or mismatched family,
   capability, or provider records fail before any activation boundary.
5. Runtime loading accepts only an XTP v40 match, decodes a bounded candidate,
   binds its identity to the authority, materializes typed rows, and invokes
   independent TargetPlan verification. V40 is a breaking hard cutover from
   v38 and all earlier XTP schemas. It requires SemanticPlan schema 34 and
   TargetPlan schema 40 after the Xi opcode registry compacted away the
   non-lowerable bounds-guard row; no compatibility alias or reserved hole is
   accepted. It preserves the exact 144-byte dynamic-entry
   expectation section and preserves the compact instruction stream introduced
   by v34. It additionally carries exact Array HOF result/callback authority,
   immutable per-instruction debug facts, and the first exact coroutine lifecycle
   root-map/root-slot and normal plus `CANCEL|EXIT` release-cleanup rows.
   Builder and verifier independently rebuild sorted lifecycle projections
   from frozen Semantic identities under the shared checked-work ceiling;
   neither scans the full entity or operation table for each row. Missing,
   duplicate, extra, reordered, or mutated lifecycle authority is rejected
   before materialization. The `INSTRUCTIONS` directory entry has
   `COMPACT` flags, zero row size, the expanded canonical TargetPlan row count,
   and the compact stream byte length. Canonical ULEB128, signed ZigZag
   immediates, and the sole format registry admit only `CONST+RETURN`,
   `PARAM+RETURN`, and `CALL_DIRECT+RETURN` super tokens; every other row uses
   the primitive token. Materialization expands all tokens into the unchanged
   canonical 32-byte TargetPlan rows before independent verification, so no VM
   consumer observes a super token. Overlong, noncanonical, truncated,
   overflowing, unknown, count-mismatched, trailing, or primitive spellings of
   registered super pairs fail in candidate decoding. There is no v36 reader,
   translation, or dual path. V31 was a breaking hard cutover from
   v30 and all earlier schemas. It adds the exact owned String range-slice
   authority: frozen receiver, two ordered signed bounds, dynamic result slot,
   tail call, representation adapters, and C recipe must all agree. V30 was a
   breaking hard cutover from
   v29 and all earlier schemas. It adds the exact scalar `Array` allocation
   authority: element storage, element count, allocator capability, and emitted
   runtime symbol are re-derived independently and must agree. V29 changed the
   instruction row to carry the generated
   target opcode as an unsigned 16-bit stable ID while preserving the exact
   32-byte wire row; the prior one-byte carrier is rejected, not widened by a
   compatibility reader. V28 combined exact `String.runes()` result-storage
   with sealed `Iterator<rune>.hasNext()` and `.next()` call authority in one
   generation; `.next()` is admitted only from the unique frozen `String.runes()`
   producer and carries a native rune result. The same generation also freezes
   the exact native-u32 `rune.toUInt32()` call and boolean `rune.isWhitespace()`
   call reached from that `.next()` row.
   V21 added an exact semantic field-name identity
   to every Target aggregate field row. V20 added the
   exact ADT-enum storage and constructor authority described above and the
   expanded caller/callee representation fields in call-argument rows. V19
   and all earlier schemas are rejected rather than reinterpreted.
   The `Channel.close()` receiver is a dispatch target rather than an argument
   or slot, and its descriptor grants no general method ABI or execution
   authority. V17 also
   includes the optional canonical instruction table in the exact digest and
   typed-row codec. A function with no instruction rows has no execution
   authority; loading it is not an empty successful program. The loader
   returns only an immutable verified TargetPlan and performs no callback,
   provider lookup, registration, or execution.
   Dependency-bearing SOURCE_EXPORT artifacts require the internal ordered
   module-set materializer. It re-verifies exact dependency module and
   semantic fingerprints plus public export/callee stable IDs before freezing
   the TargetPlan. The standalone installed runtime load route does not accept
   this vector and therefore remains fail closed for cross-module artifacts.
6. Installed runtimes construct artifact authority only from exact XSM bytes.
   XSM decoding applies the current schema, operation-registry fingerprint,
   payload digest, ownership replay, semantic fingerprint, and independent
   SemanticPlan verifier before the runtime builds its canonical native
   TargetProfile. They do not guess SemanticPlan or TargetProfile authority
   from a host name, file name, sibling artifact, or caller-authored
   fingerprint. A dependency-bearing schema-23 XSM cannot be decoded as a
   standalone artifact: its module-set route must present the canonical ordered
   vector of every exact verified dependency SemanticPlan, and the decoder
   rechecks dependency module IDs, semantic fingerprints, public export IDs,
   callee function IDs, and frozen suspendability. The installed sole-scalar
   route remains standalone-only and therefore rejects such cross-module
   artifacts.
7. The installed positive route is deliberately limited to exact XSM plus its
   matching XTP and the sole-function scalar generation executor. A build-time
   fixture generator freezes exact bytes for the current native profile; the
   executed archive test links only the installed runtime archive, constructs
   authority from XSM, materializes the verified XTP, prepares and activates
   one generation, and executes function 0 to its exact scalar result. The CLI
   exposes this same governed product route only as
   `xray run module.xtp --semantic-plan module.xsm`. It identifies both inputs
   by bytes, runs the public runtime authority/load/generation APIs, prints the
   scalar result, and completes drain, retire, unload, and authority teardown.
   Optional `--timings` reports artifact-read, semantic-verify, target-verify,
   activation, and entry/output monotonic durations for this exact route only;
   it cannot decorate source or legacy execution. No XSM or XTP encoder enters
   the runtime archive. Plans with exports, calls, roots, storage, allocation,
   adapters, or coroutine execution authority still fail PREPARE; this route
   is not general module activation.
   Standalone XRC generation and execution are retired product routes. The
   compiler rejects default `.xrc`, `bytecode`, and `bc` outputs before isolate
   creation, while retaining one explicit offline C container for compiler
   development. Header-format aliases are retired rather than preserved as a
   second spelling. The runtime recognizes valid XRC magic only to reject it before
   isolate creation; renamed artifacts cannot bypass the cutover.
   The retired `.xrc` bytecode-embed runner and its CTest fixtures are not a
   parallel product route: the exact XSM/XTP runtime-archive gates above are
   the sole positive embedded-artifact coverage.
8. Decoder and materializer diagnostics retain their governed stage code at
   the public API and CLI boundaries. They are not folded into a generic
   schema or unverified-artifact error.
9. Legacy product residue is a monotonic ceiling: complete owner removal and
   terminal zero are valid, while new owners, tokens, symbol names, or count
   growth fail. The installed archive gate fails when inspection is
   unavailable, rejects compiler and `xi_` symbols, and treats any new `xvm_`
   symbol as uninventoried legacy residue.
10. The installed SDK is not a recursive copy of compiler, VM, or runtime
    headers. Configure derives the exact quoted-include closure of every
    generated-C entry header, rejects legacy product paths and text while
    deriving it, and emits a digest-bound closure manifest. The installer
    publishes exactly that manifest's public and private header set. Both the
    raw installed-evidence producer and the final completion checker rescan
    the published tree and reject missing, additional, or digest-mismatched
    SDK files. In particular, the old VM umbrella/API, native-module SDK,
    opcode definitions, isolate internals, and proto container codec are not
    installed aliases or compatibility surfaces. Payload manifest schema 2
    carries no legacy bytecode or native-module ABI version fields; schema 1
    is rejected rather than reinterpreted.

anchor-sha256: CMakeLists.txt 7f0b34bbbe209405f605d9a1a03d83e3913dd641c7a69b78e0faa3d68b439827
anchor-sha256: include/xray_target_plan_load.h b4908c5917da540471ca4093eacd3dc231f465362d0f126da24700d0404def42
anchor-sha256: src/plan/format/xr_artifact_kind.h cfd9c31f2e84040413d9b42889371867fad1a5a7f61e7d2066a69e687463318d
anchor-sha256: src/plan/format/xr_artifact_kind.c a4569b3d3bcc67e28bc025f510ddb1dd95c4725e07ea7df59f93c56bb2f884b5
anchor-sha256: src/plan/format/xr_xsm_schema.h 98fc9a9c8f4627de81075e25905a55189ce82f5b985b190a6bfaa6ce72810242
anchor-sha256: src/plan/format/xr_xsm_decode.c c01716de9e88323ea8fe6eb72d61911cd6a2bc54a40b6755b7544a4018fb86b1
anchor-sha256: src/plan/format/xr_xtp_schema.h e452a27b2149e30bbafded2799a0a3e2a51fa9df7ffcdbcd41556bde2f230601
anchor-sha256: src/plan/format/xr_xtp_internal.h 1ee09126e21b001fe48425c0bd3568c0dc6fad168343fd106e731beb7d01217e
anchor-sha256: src/plan/format/xr_xtp_artifact.c ed8328a99f27b5bbed4b0a0909f0e42c67ebfff066e80e1bdd4ea01439ebf9d1
anchor-sha256: src/plan/format/xr_xtp_decode.c 9ccebe5d3887a58cdb8746861edeee2e6cc2128b028dccfc2d1387c0127bb014
anchor-sha256: src/plan/format/xr_xtp_encode.c c5131d9c1ec60d2d19729d21d95dfc013194d92fe88a49b0d9ad6fed63ab1a9b
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.h 39a81bcf5b337b7fdbf4aafaa4eb8a6ba575d4a1853f6f86c8dca6d0d2e0579a
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.c d2c219ad22c0f22193abe31436ba5466f7d60b3c4511628a7fa14a0fb2a98773
anchor-sha256: src/plan/format/xr_xtp_row_fields.h 84e5b18d06b0a44e25708b80e0f19ff70918d0babd988d0d9ea7260fcb842f29
anchor-sha256: src/plan/format/xr_xtp_rows.c 7e2c7c25d880a3f0d38abf7a48e63f9918c78eaf68e2734b9d0d68bb575abbcd
anchor-sha256: src/plan/format/xr_xtp_text.h 63367e2a75cc5e1511d1980cd82f579863cfd86a97cbf47d892f4c945d4ca0e1
anchor-sha256: src/plan/format/xr_xtp_text.c 3fd7546507f2fdd20f92e1151b2a1bb79d11a816d61c9aae2f30d69b9ac8c68b
anchor-sha256: xisa/target/xtp_super_ops.def 20968dd05c20d4caa85172fb2fc8cc051b74a1c6dcf93534368ce3ca7e491f88
anchor-sha256: src/runtime/abi/xr_target_machine_facts.h 8c8d1c341fb4639bb47c982ac6dfd851571d154823101e00011a63fcb14486d8
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.h 5ea9aa4ff63d88b62dbf1f43bea9ab2875d9d63ef2722d73df5a71c59eedda1b
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c 6487ae39149a4ea28fab04bdf2ccbf88c0bccaded75cf20376b38c8447dbdd1c
anchor-sha256: src/plan/target/xr_target_profile.h 311d38da384e02be242e4025a7d14d7ee253c0a1b71d7af502c937d3ceae30b1
anchor-sha256: src/plan/target/xr_target_profile.c aa5b7db6be962e0deaedd2de4ae105f87f777d392fbf30e72b4da8704efa248f
anchor-sha256: src/plan/target/xr_target_plan.h e4af616460a925bd1ad679135860ef346d6028c47b2a819ab570ba7a3562cae6
anchor-sha256: src/plan/target/xr_target_plan.c c43c90f7f4b59ad2864b9666863288f030979dd085075dc5c18f98f361334f51
anchor-sha256: src/plan/target/xr_target_builder.c 770ffc0b0eef39706ce824ca79fa042aaa1f9dc0c093a909a2da2e7992e40b4d
anchor-sha256: src/plan/target/xr_target_verify.c bf304afc834f4f08f9fdcded3ab176df619754daa02c737d8e728878324c7ac8
anchor-sha256: src/plan/target/xr_xtp_materialize.c 638e5cb5fb73d8979a0c8f35f240800ac00d588ac4bad26889d0284391914eb4
anchor-sha256: src/runtime/xr_runtime_artifact_authority_internal.h 9bf3dbbd4ad323ee8a7745fce137c49b3a50992dc9a443c1851d95ec8a048e2b
anchor-sha256: src/runtime/xr_runtime_artifact_authority.c eca95f69c7cf1e562ddd50b39787f7fe6e842c9e99f04baf72419854060ad317
anchor-sha256: src/runtime/xr_runtime_artifact_verify.c aa42e2ea69d8e2669f1019905a213c62002e9c0d07d4e5df14e0758d8fc14c4a
anchor-sha256: src/runtime/xr_target_plan_load.c 968b2292134379c097e034376aa6eb5dc322992e1c24b31d003436e1a6aa5aec
anchor-sha256: src/app/cli/xcmd_run.c b153d14afb35e220ccacd6c941a4869fb3fcddc1ba7807f7b7ff2c4b5ef503b9
anchor-sha256: contracts/target-machine/legacy-product-residue.json ddbc46a5e2a8c1f88e8707ec6b7e917b1e58ef4f23a565238c70b24889a1f358
anchor-sha256: scripts/check_legacy_product_residue.py c3f15f8812355cb1bd3b316137d5cabc08ad2d915a1c92431c018923842bc327
anchor-sha256: tests/unit/plan/test_target_plan.c cc0968d0619513fa2eab95b493235510dbf4870428e17b79cac33536ca2b0b53
anchor-sha256: tests/unit/plan/test_xtp_format.c 50cfba1053b1e203996701010e0067915af3ace898e422b5c4ae2de8d9f49c70
anchor-sha256: tests/unit/plan/test_xtp_resource_stress.c cfe41d4e83103cadb5e8eabc7a48aef121b5dc5ebdee14939e5c0f80bf955fff
anchor-sha256: tests/unit/CMakeLists.txt 08db8bd5e51152ec5afcc1aff72b9a9c29fea351619522fbbcf340b873a673dd
anchor-sha256: tests/unit/runtime/test_runtime_target_plan_load_archive.c 68cf5903360638cba7fe872f690810dfe7a9b531ee33d9939f347e1080b94d20
anchor-sha256: tests/cli/run_target_artifact_boundary_tests.py a5276ef24b09626b078702d2c00e7f957fd686b20b11757dbb623342710d1849
anchor-sha256: tests/cli/run_plan_command_tests.py 44a924d4d39b558c0e53a04080ea3fd42071044039ad3f2de539d9d1e6299f0f
anchor-sha256: tests/fuzz/fuzz_xtp_decode.c 8ef332c992bb8e44a2dbe06bd5463458ff84df41d9088d0596ace17e5e806d94
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 70d40dfa429c78f663381887bf4676c2b68c97334c55344554a6da587e886be8
anchor-sha256: src/plan/target/xr_target_entry_abi.h 80cd119cbc095ddfddbf95ff5085fbaa23659256feb8d18a36e43416013747ea
anchor-sha256: src/plan/target/xr_target_entry_abi.c cb5cd57a0b8f3bbfe2123a07f583da997d7d2989e5158fd241406b96ce433b12
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
