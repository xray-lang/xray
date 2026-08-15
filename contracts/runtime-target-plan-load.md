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
   copy with checked arithmetic.
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
   local child. Builder, Target verifier, and AOT materialization verifier
   independently reconstruct the store/load dominance, unique child, exact
   signature, slot, ownership/provenance, and complete use set. It grants no
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
   authority.
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
   Source-class-instance storage covers the three values a construction
   produces: the borrowed read of the class object the call dispatches on, the
   owned instance the call returns, and the borrowed reads of that instance out
   of its own module slot. All three are outer tagged dynamic values for the
   same reason the class object is, and ownership is never a property of the
   family: it is the operation's own result ownership, owned for the
   construction and borrowed for either read, so a read can never be frozen as
   an owning root. The construction itself carries a call dispatch kind driven
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
5. Runtime loading accepts only an XTP v30 match, decodes a bounded candidate,
   binds its identity to the authority, materializes typed rows, and invokes
   independent TargetPlan verification. V30 is a breaking hard cutover from
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

anchor-sha256: CMakeLists.txt 989526ba9dfad5b70b4385edbd2fef11bdb9b63ed36235d8814e37a13521438f
anchor-sha256: include/xray_target_plan_load.h b4908c5917da540471ca4093eacd3dc231f465362d0f126da24700d0404def42
anchor-sha256: src/plan/format/xr_artifact_kind.h cfd9c31f2e84040413d9b42889371867fad1a5a7f61e7d2066a69e687463318d
anchor-sha256: src/plan/format/xr_artifact_kind.c a4569b3d3bcc67e28bc025f510ddb1dd95c4725e07ea7df59f93c56bb2f884b5
anchor-sha256: src/plan/format/xr_xsm_schema.h 98fc9a9c8f4627de81075e25905a55189ce82f5b985b190a6bfaa6ce72810242
anchor-sha256: src/plan/format/xr_xsm_decode.c 268dc519387d24839794f2b64cb4a1c7fed2cfff9c72bb6f81485c9561940262
anchor-sha256: src/plan/format/xr_xtp_schema.h 4f4429e80c39784f75dd5035864aa506af6710db0c29ceaaf9df97cec9140744
anchor-sha256: src/plan/format/xr_xtp_internal.h 2d1a76e7dd0a7d1f623ce3fc8118c4235d2694e03cceea8db756ec8f67e3a346
anchor-sha256: src/plan/format/xr_xtp_artifact.c ed8328a99f27b5bbed4b0a0909f0e42c67ebfff066e80e1bdd4ea01439ebf9d1
anchor-sha256: src/plan/format/xr_xtp_decode.c debe51ef7fd15e75b2b120642e7d4d69518784bb59b747503ea37f0e3b332d04
anchor-sha256: src/plan/format/xr_xtp_rows.c 37ade66cec19c828eefe6ed2066273fe16197b9f7908f2b4977e73eb39851c41
anchor-sha256: src/runtime/abi/xr_target_machine_facts.h 8c8d1c341fb4639bb47c982ac6dfd851571d154823101e00011a63fcb14486d8
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.h 5ea9aa4ff63d88b62dbf1f43bea9ab2875d9d63ef2722d73df5a71c59eedda1b
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c 6487ae39149a4ea28fab04bdf2ccbf88c0bccaded75cf20376b38c8447dbdd1c
anchor-sha256: src/plan/target/xr_target_profile.h ff5b5de806ff2eb1b65d522b58f9da2bbf421f8f6c9f5ff79235dcba76c64ab9
anchor-sha256: src/plan/target/xr_target_profile.c aa5b7db6be962e0deaedd2de4ae105f87f777d392fbf30e72b4da8704efa248f
anchor-sha256: src/plan/target/xr_target_plan.h d1ed20e9afa4b5c1bb361a939a8c5b627c6d4e814b68f3303b8a05454bfa5c99
anchor-sha256: src/plan/target/xr_target_plan.c c170ef7ebf27a9969d68e091d65169d466b87f6212c2e155f7a25759e35a041c
anchor-sha256: src/plan/target/xr_target_builder.c 49bf35aaf43ede50958e11cefa4941076b9f64ac319ed509d0a16939495e279f
anchor-sha256: src/plan/target/xr_target_verify.c 4bf286fac32e746ff90201118731b94f2eccf8883ac37b2dd28f16984504eac1
anchor-sha256: src/plan/target/xr_xtp_materialize.c 02de4138a0d49d1afd6143cec910cbe1061a6d84d82096d48fa4800852b98267
anchor-sha256: src/runtime/xr_runtime_artifact_authority_internal.h 9bf3dbbd4ad323ee8a7745fce137c49b3a50992dc9a443c1851d95ec8a048e2b
anchor-sha256: src/runtime/xr_runtime_artifact_authority.c eca95f69c7cf1e562ddd50b39787f7fe6e842c9e99f04baf72419854060ad317
anchor-sha256: src/runtime/xr_runtime_artifact_verify.c aa42e2ea69d8e2669f1019905a213c62002e9c0d07d4e5df14e0758d8fc14c4a
anchor-sha256: src/runtime/xr_target_plan_load.c 507103831c2693d9965d44652761a816bdd60ec6c1b9f21919a0bb60dc5e70e9
anchor-sha256: src/app/cli/xcmd_run.c 4b3e9f0c7bc8068ecbbbb68950800928e33db5df49ffd0cace04a22ba96b2c41
anchor-sha256: contracts/target-machine/legacy-product-residue.json 577aa49d6502b2b1cf3a88191851bbbc82775fe2e53f51ec7ea83ca58281a548
anchor-sha256: scripts/check_legacy_product_residue.py d160f8b9ab1d16da893bcc30a7ed90d583dda9e478dd11f67c9ce299629f8d2f
anchor-sha256: tests/unit/plan/test_target_plan.c 606990c62adcfbc722b9930e05a8ad6919291fd67e864c8a567945d432e9e128
anchor-sha256: tests/unit/plan/test_xtp_format.c 9654ba47989b6b34e4e40f6196a02bb95818fd6f83aa5f4a4f78e6907bf4bd31
anchor-sha256: tests/unit/CMakeLists.txt b6b991e6f4791ec8473e9dd26fa3cab4d03f6f9959add218b2db305f100e8a17
anchor-sha256: tests/unit/runtime/test_runtime_target_plan_load_archive.c 68cf5903360638cba7fe872f690810dfe7a9b531ee33d9939f347e1080b94d20
anchor-sha256: tests/cli/run_target_artifact_boundary_tests.py 6df46f3edff17f29786f16df95615187f34b9da5283f79228c06c0cfe3f1598d
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py adbe96cb24fd2da66f8e8c3148d78eab7f59d3b2544ba07a87f41540b6f760cd
