# Runtime TargetPlan artifact load contract

This contract freezes exact XSM authority construction and the TargetPlan load
boundary. Low-level loading itself remains non-executing; the separately
governed generation lifecycle executes the ordinary sole-function scalar route
and one exact bounded two-module direct-`i64` program graph. This contract does
not claim general program graphs, dynamic reload, name-based program exports,
roots, or general product activation.

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
   The shared native-hosted TargetProfile projection mechanically consumes
   this same runtime authority and adds no code-generator features, defaults,
   or caller-authored machine facts. Product compiler sessions install that
   exact frozen projection; a missing or conflicting profile fails closed.
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
   aggregate family admits the bounded leaf-value direct-local family only
   through SemanticPlan 43 program provenance and typed type, field, function,
   and call bindings. It derives field geometry from the verified target profile
   and declaration ordinals, binds caller and callee to one trivial aggregate
   representation, and records the return in caller storage with no adapter or
   ownership transfer. Builder and independent verifier reconstruct the joins
   separately; a missing binding, shape-helper fallback, Xi guess, or source-name
   special case is not load authority. The projection alone grants no execution:
   the bounded leaf VM/AOT family additionally requires the exact independently
   verified instruction group frozen by the typed execution contract. A loaded
   plan with layout rows but without that group remains execution unavailable. The
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
   Range-slice view storage covers the borrowed view `container[start:end]`
   produces. The view holds a pointer and a length and never a tagged carrier:
   it borrows part of a container it did not allocate, so there is no allocation
   behind it to hold and no tagged form to box it into. The element must be an
   exact scalar, because its stride is what turns the length into bytes and a
   reference-carrying element would leave a reference-count obligation behind
   the borrow that this family does not discharge. The container operand keeps
   whatever carrier its own family bound, which is what leaves the row
   adapter-free. Builder, independent verifier, and AOT representation
   refinement read one judgement. The family grants no container allocation,
   bounds check, element traffic, root, or cleanup authority.
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
   Source-class-instance storage covers the four local values an instance can
   be, plus the owned result of an exact imported construction:
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
   read can be named. The construction itself carries a call dispatch kind
   driven by the SemanticPlan call target. A local target names no dependency
   or callee. An imported target instead carries the exact ordered dependency,
   explicit source-class export identity, and constructor stable identity while
   keeping the target-local callee index absent. Target construction and its
   independent verifier mechanically match those frozen identities and the
   external result class; neither re-walks the Semantic initializer graph or
   treats the import binding as a local shared class slot. The construction
   never makes its caller suspendable, because the shared judgement admits only
   a call whose effects are the generated call effects.
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
   A direct-local borrowed `ref Array<T>` argument is admitted only when the
   frozen Array child has exact scalar storage or resolves to one frozen
   source-class declaration. The latter maps to `TAGGED` element storage;
   parameter and operand rows remain the sole ownership and transfer
   authority. Builder and verifier derive that judgement independently from
   SemanticPlan, while String and other reference-capable children continue to
   fail closed at this element-indexing boundary.
5. Runtime loading accepts only an XTP v52 match, decodes a bounded candidate,
   binds its identity to the authority, materializes typed rows, and invokes
   independent TargetPlan verification. V52 is a breaking hard cutover from
   v51 and all earlier XTP schemas. It requires SemanticPlan schema 43 and
   TargetPlan schema 52, including exact PSC v6 provenance, typed program
   bindings, and the direct-local scalar-ref v1 call-row interpretation; no
   compatibility alias is accepted. It
   preserves the exact 144-byte dynamic-entry
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
   Dependency-bearing SOURCE_EXPORT artifacts require exact program-module-set
   authority. It re-verifies exact dependency module and semantic fingerprints
   plus public export/callee stable IDs before freezing the TargetPlan. The
   ordinary single-XSM authority constructor does not accept this vector and
   therefore remains fail closed for cross-module artifacts. The bounded scalar
   module-graph family instead uses the same public TargetPlan loader with
   PROGRAM_MODULE_SET authority, which selects the unique program-graph
   materializer. TargetPlan schema 52 carries one program-graph row and canonical
   module partitions over global TargetPlan tables; each partition identifies
   its SemanticPlan through the full canonical program module set and records
   pointer-free row ranges. `MODULE_PARTITIONS` uses fixed 208-byte rows and has
   a hard limit of 256 rows; `PROGRAM_GRAPHS` uses one fixed 340-byte row and
   has a hard limit of one row. The materializer rejects ordinary/graph
   directory shape substitution, locates the entry from the graph and partition rows,
   verifies the complete ordered SemanticPlan set and aggregate semantic
   fingerprint, rebuilds the entry's direct dependency vector, freezes one
   plan, and reruns independent TargetPlan verification. Reordered, duplicate,
   missing, foreign, or re-signed module, graph, partition, call, argument, or
   fingerprint authority fails closed. Canonical graph encode/materialize/
   re-encode is byte-identical. The ordinary materializer rejects this
   multi-SemanticPlan authority; `xr_runtime_target_plan_load` selects the graph
   materializer only from the already verified program authority. A plan produced by the graph
   materializer is executable by the typed VM only through its verified graph
   entry and same-plan global rows. Its warm VM route uses the existing decoded
   cache only after that cache is bound to the exact runtime generation,
   canonical module-set fingerprint, program fingerprint, and GCI; the cache
   does not copy a fixed two-partition schema or construct per-module plans.
   The public `XrProgram` facade admits only the exact currently installed
   two-partition/two-function/one-call/one-argument direct-`i64` capability. It
   obtains the unique entry from the verified graph row, reuses the same
   generation, live manifest, decoded cache, Program TargetPlan, program and
   module-set fingerprints, and GCI, and rechecks that complete identity on
   every execution. It has no function-index, module-name, export-name, or
   ordinary-module recovery path. The compiler's bounded source-AOT lane may
   consume the same plan through its independently verified program C-emission
   binding for the exact two-module scalar graph only; it constructs no
   per-module executable plans. Neither installed route may infer compiler-only
   authority or reconstruct a graph from module names.
6. Installed runtimes construct artifact authority only from exact XSM bytes.
   XSM decoding applies the current schema, operation-registry fingerprint,
   payload digest, ownership replay, semantic fingerprint, and independent
   SemanticPlan verifier before the runtime builds its canonical native
   TargetProfile. They do not guess SemanticPlan or TargetProfile authority
   from a host name, file name, sibling artifact, or caller-authored
   fingerprint. A dependency-bearing schema-43 XSM cannot be decoded as a
   standalone artifact: the currently admitted module-set route requires the
   exact two fragments of this bounded capability and canonicalizes arbitrary
   input order only from their verified program-module rows. The decoder
   rechecks dependency module IDs, semantic fingerprints, public export IDs,
   callee function IDs, and frozen suspendability. The ordinary installed route
   remains standalone-only and rejects such cross-module artifacts; the
   `XrProgram` route instead requires the complete verified canonical set. The
   module-set decoder enforces overflow-safe aggregate encoded and retained
   decoded-storage budgets. Independent verification scratch remains governed
   by the existing per-table and semantic-count limits and is not described as
   part of that retained-storage budget.
   Runtime artifact authority schema 3 distinguishes ordinary and program
   authority. It freezes authority kind, canonical semantic-module count, exact
   entry semantic fingerprint, program fingerprint, canonical program-module-set
   fingerprint, and 16-byte GCI into the authority fingerprint. Ordinary
   authority requires count one and zero program fields;
   program authority requires the exact nonzero program fields. Candidate and
   materialized-plan binding independently join them, and schema 2 is not read.
7. The installed positive routes are deliberately limited to exact artifacts
   and the two executors above. A build-time fixture generator freezes exact
   bytes for the current native profile. The archive gate links only installed
   headers plus the installed runtime archive. It first proves the ordinary XSM
   plus matching XTP route, then performs two independent public program loads.
   The first loads the two program XSM images in canonical input order, executes
   the graph entry to 42, and unloads. The second loads the same images in reverse input order,
   executes two concurrent calls to 42, joins them, and only then unloads and
   destroys the runtime. Canonical order comes only from verified provenance
   rows. The CLI exposes only the distinct ordinary single-module route as
   `xray run module.xtp --semantic-plan module.xsm`. It identifies those inputs
   by bytes, runs the public runtime authority/load/generation APIs, prints the
   scalar result, and completes drain, retire, unload, and authority teardown;
   it does not yet expose the program facade.
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

anchor-sha256: CMakeLists.txt 34b5d2ab2fa1c67c1152dbc0e3d849b624d0b3bc6275bd1465647d40e7222b19
anchor-sha256: include/xray_runtime_api.h 07d75f03ee3baea4301bf30ac67c15b77d74feaec28322fb818c247970a1ce44
anchor-sha256: include/xray_target_plan_load.h cd91018657a5c4af0ff07b2a56ec189a679ccf8b4551aaaa444e5ba6214df581
anchor-sha256: src/plan/format/xr_artifact_kind.h cfd9c31f2e84040413d9b42889371867fad1a5a7f61e7d2066a69e687463318d
anchor-sha256: src/plan/format/xr_artifact_kind.c a4569b3d3bcc67e28bc025f510ddb1dd95c4725e07ea7df59f93c56bb2f884b5
anchor-sha256: src/plan/format/xr_xsm_schema.h f5e6d875255f73803545a9cf99450e6b140e6282ee19233048afd4e0ce41362b
anchor-sha256: src/plan/format/xr_xsm_decode.c b31bf1696bacd3b435ea1383da4f92df51bb6692c45f28e7d22ab829154db8f4
anchor-sha256: src/plan/semantic/xr_semantic_plan.h bd864991027a87d48222a4eab6afc79ee6758be466e55caa8c6f3c06ffcef670
anchor-sha256: src/plan/semantic/xr_semantic_plan.c aeaad12aee601f2bfb982fbaa72484a3845b8382874fe84a86c88c1594458eb1
anchor-sha256: src/plan/semantic/xr_semantic_verify.c f255b5868f54b94bcb2c95a601d94eb30fcfb10730a7962d2d527acb45fdaf1f
anchor-sha256: src/plan/format/xr_xtp_schema.h f4e702190946300701eebc213774c668a1f731f22d4f87b446c00fb2cd48077f
anchor-sha256: src/plan/format/xr_xtp_internal.h 35ac710feb01cabdd9de87b17a481aa73847984f8c4e26354d6902344879058f
anchor-sha256: src/plan/format/xr_xtp_artifact.c ed8328a99f27b5bbed4b0a0909f0e42c67ebfff066e80e1bdd4ea01439ebf9d1
anchor-sha256: src/plan/format/xr_xtp_decode.c 9ccebe5d3887a58cdb8746861edeee2e6cc2128b028dccfc2d1387c0127bb014
anchor-sha256: src/plan/format/xr_xtp_encode.c 4db5c593d44645c7d142f938973ccb199ba2a9d0c21dbfc539f17e05c4bac74e
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.h 39a81bcf5b337b7fdbf4aafaa4eb8a6ba575d4a1853f6f86c8dca6d0d2e0579a
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.c d2c219ad22c0f22193abe31436ba5466f7d60b3c4511628a7fa14a0fb2a98773
anchor-sha256: src/plan/format/xr_xtp_row_fields.h 4464709ecaf6f74067c2cc4ce58b3449c684960d88ed744ed803fb8c53a65474
anchor-sha256: src/plan/format/xr_xtp_rows.c 0c3408c70bb44b1049849be5a43ca4989187eebfd10781ea5f371625a35f0789
anchor-sha256: src/plan/format/xr_xtp_text.h 63367e2a75cc5e1511d1980cd82f579863cfd86a97cbf47d892f4c945d4ca0e1
anchor-sha256: src/plan/format/xr_xtp_text.c 794c85faec54254597eb2cc989b3d0a761e794988105d6bdc18aa19d82ac4162
anchor-sha256: xisa/target/xtp_super_ops.def 20968dd05c20d4caa85172fb2fc8cc051b74a1c6dcf93534368ce3ca7e491f88
anchor-sha256: src/runtime/abi/xr_target_machine_facts.h 8c8d1c341fb4639bb47c982ac6dfd851571d154823101e00011a63fcb14486d8
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.h ec7f8aba2e76b4e2a10f898468f316c2f36863e8f35a7cb335ad9a30b75974e3
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c d0836e34b01e009e2a260b4c4c022b6a7f7a4e9ce80e77f18fc23b2d0e912ce2
anchor-sha256: src/runtime/abi/xr_runtime_target_profile.h 8653f30d2ed073fd75d3adab9eb5e0cb27ddf538b7e0634f896ce21704382308
anchor-sha256: src/runtime/abi/xr_runtime_target_profile.c 31918070b7e530780073a8ca0010d2b62f78afbfcdf4d610c41bf27e4da4a5d1
anchor-sha256: src/plan/target/xr_target_profile.h ccac7cd6d7ecdcb5cf4a109f5582b4e2e17bc6bdc872e3779ac4f179a8aff0d1
anchor-sha256: src/plan/target/xr_target_profile.c aa5b7db6be962e0deaedd2de4ae105f87f777d392fbf30e72b4da8704efa248f
anchor-sha256: src/plan/target/xr_target_plan.h 657482101ff9a675d6de4790ae1240479291d06c9a9a1a04ff555c5787c06381
anchor-sha256: src/plan/target/xr_target_plan.c 863cc34f128bc05819fee19648ba3d71becd713628df26bffc82338b7326226c
anchor-sha256: src/plan/target/xr_target_builder.c 458764430ef1bf457dbc75a20dff1f784771e3cef9dbbe399d923f371e53d58b
anchor-sha256: src/plan/target/xr_target_verify.c ceec495fc3197b7f2622b5420695028aee02801830f75e86b67a6444ed825980
anchor-sha256: src/plan/target/xr_xtp_materialize.c 6079934e95208abe3b7b7251b4c4b59275c61776f20de0d0873b70617899a62a
anchor-sha256: src/runtime/xr_runtime_artifact_authority_internal.h 5e81f18c79504cd7876910b0ad0d88b270fd19fbc6de7a44d5daa2bf47263692
anchor-sha256: src/runtime/xr_runtime_artifact_authority.c 47203f0c178dc46872e6d6ec8c680a8e0cbd0f5e4670e7a4380f030b2d61a043
anchor-sha256: src/runtime/xr_runtime_artifact_verify.c 6769b535c81ef682bfec16f7364b2fa5991e037ed7f2e67e70b21d12f331c4a0
anchor-sha256: src/runtime/xr_target_plan_load.c 162babb92d90b8ead7842e68de5a6bccbb0a304e3e62299de4bb64c8ccf7a22d
anchor-sha256: src/runtime/xr_runtime_api.c 2297cb107d76409c13d0bb8019084722cb573bb79d5bce5b85613a32b45cf14a
anchor-sha256: src/app/cli/xcmd_run.c 4bbdfcfe6426abc90b411dd271bb849297a930e3c6364692225d8ba08e1c2f98
anchor-sha256: contracts/target-machine/legacy-product-residue.json c335bd1360bdbd242d642a4ef5990072a2111345daf237e87cb4af103967f230
anchor-sha256: scripts/check_legacy_product_residue.py 0d8b95a014d23f7732e46b837f8c8d1cda3406da1464b314e6d2f401bd2a3705
anchor-sha256: tests/unit/plan/test_target_plan.c cb80fcdc5bfbefdae5f9f10557e8c6979e426e51f1da14a7e7d87e9d3e4eecdb
anchor-sha256: tests/unit/plan/test_xtp_format.c c85b9891b26f3ab4043df7e411ce605c16b6d4fb63a8bc5e14cb7bf87c1db352
anchor-sha256: tests/unit/plan/test_xtp_resource_stress.c 48957cbd5b000fb267af4e5ac456223161afccc8c0e9a5b12102a75a236d7124
anchor-sha256: tests/unit/frontend/test_xa_program_semantic_closure.c 570675d165cc6cdd58bd82ca089cbf73a7a8106cfc5d8c66087350d13b93864e
anchor-sha256: tests/unit/CMakeLists.txt 2f5bcd0c92d910e742ad57f1130dc6a38f344a507486f0a5217b7d89ce5857e4
anchor-sha256: tests/unit/runtime/test_runtime_target_plan_load_archive.c 30015dd2f75ad8917788a30b367f203d15e85d037af8d394940a4d30af87e69a
anchor-sha256: tests/cli/run_target_artifact_boundary_tests.py ac10e972dbd1c43784f78fa5746c5820b999529830b223f83ec3ebbf421e095f
anchor-sha256: tests/cli/run_plan_command_tests.py 44a924d4d39b558c0e53a04080ea3fd42071044039ad3f2de539d9d1e6299f0f
anchor-sha256: tests/fuzz/fuzz_xtp_decode.c 8ef332c992bb8e44a2dbe06bd5463458ff84df41d9088d0596ace17e5e806d94
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 536faa4afd6b0374ccc17e9446320a43eeea92f5c696fbd5f6dac644e95c7d53
anchor-sha256: src/plan/target/xr_target_entry_abi.h 80cd119cbc095ddfddbf95ff5085fbaa23659256feb8d18a36e43416013747ea
anchor-sha256: src/plan/target/xr_target_entry_abi.c cb5cd57a0b8f3bbfe2123a07f583da997d7d2989e5158fd241406b96ce433b12
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
