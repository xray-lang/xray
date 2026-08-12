# Runtime TargetPlan artifact load contract

This contract freezes exact XSM authority construction and the TargetPlan load
boundary. Loading itself remains non-executing; the separately governed
generation lifecycle may execute only the sole-function scalar route. This
contract does not claim provider registration, an entry cell, exports, calls,
roots, or general module activation.

1. Artifact identity comes from bytes. The probe reports match, need-more,
   unknown-reserved, or extension conflict. XSM and removed XTP prefixes are
   resolved before legacy XRC, and XRC requires its full magic plus exact
   format version. A file name can only confirm an identity or conflict with
   it; it cannot create one.
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
   storage, Channel-receive storage, direct-local-GO-callee storage, and
   SOURCE-namespace storage. The
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
   direct-local call target. It grants no closure allocation/body, root map,
   cleanup, general indirect call, or executable callable authority.
   Direct-local-GO-callee storage is a separate family for the same outer
   borrowed dynamic callable token when it is produced by one canonical
   entry initializer (`CLOSURE_NEW` then `SET_SHARED`) and every use of the
   matching `GET_SHARED` is operand zero of `XI_GO` for the same canonical
   local child. Builder, Target verifier, and AOT materialization verifier
   independently reconstruct the store/load dominance, unique child, exact
   signature, slot, ownership/provenance, and complete use set. It grants no
   GO task-result, child-task object, callable body, allocation, root map,
   root slot, cleanup, argument storage, or coroutine execution authority.
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
   SOURCE-namespace storage covers only the borrowed dynamic outer `XrValue`
   for the canonical `IMPORT_REF -> SET_SHARED -> GET_SHARED` namespace token
   consumed as operand zero of exact SOURCE_EXPORT calls into one frozen
   dependency. Builder, Target verifier, and AOT materialization verifier
   independently reconstruct the ordered dependency, module identity, shared
   slot, exact import/store/load use sets, and call receiver binding. This row
   grants no imported module object body, allocation, root, cleanup, member
   lookup, argument ABI, dependency activation, or cross-module frame.
   Foundation capability masks are also exact. Allocator and panic
   requirements have dense records bound to the same canonical provider kinds.
   Missing, duplicate, additional, or mismatched family,
   capability, or provider records fail before any activation boundary.
5. Runtime loading accepts only an XTP v15 match, decodes a bounded candidate,
   binds its identity to the authority, materializes typed rows, and invokes
   independent TargetPlan verification. V15 is a breaking hard cutover from
   v14 and all earlier schemas. It preserves all v14 facts while adding the
   exact SOURCE-namespace-storage family described above. V14 and all
   earlier schemas are rejected rather than reinterpreted.
   The `Channel.close()` receiver is a dispatch target rather than an argument
   or slot, and its descriptor grants no general method ABI or execution
   authority. V15 also
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
   fingerprint. A dependency-bearing schema-17 XSM cannot be decoded as a
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
8. Decoder and materializer diagnostics retain their governed stage code at
   the public API and CLI boundaries. They are not folded into a generic
   schema or unverified-artifact error.
9. Legacy product residue is a monotonic ceiling: complete owner removal and
   terminal zero are valid, while new owners, tokens, symbol names, or count
   growth fail. The installed archive gate fails when inspection is
   unavailable, rejects compiler and `xi_` symbols, and treats any new `xvm_`
   symbol as uninventoried legacy residue.

anchor-sha256: CMakeLists.txt fc14912490a1910dd4fc5f94f978e050c50e3a1d6d213c2f8a98ea2cd6c95b85
anchor-sha256: include/xray_target_plan_load.h b4908c5917da540471ca4093eacd3dc231f465362d0f126da24700d0404def42
anchor-sha256: src/plan/format/xr_artifact_kind.h 38fd73865e25d62392d8dd0abfd5e6193edf2356803a817349db14c43ccf9874
anchor-sha256: src/plan/format/xr_artifact_kind.c 289fb506284ed97372e211225dcb5c1d205d7149416ddc96abb2a7f3b8704b39
anchor-sha256: src/plan/format/xr_xsm_schema.h 98fc9a9c8f4627de81075e25905a55189ce82f5b985b190a6bfaa6ce72810242
anchor-sha256: src/plan/format/xr_xsm_decode.c 0cf174f58b81bf5b0b3588a312a02483a2351b3efa9de875513aad4dd707c013
anchor-sha256: src/plan/format/xr_xtp_schema.h e9c410961d012cee5003c53d75ad9b22381983eeda2850f8e8f95da5e6d56dd1
anchor-sha256: src/plan/format/xr_xtp_internal.h 2d1a76e7dd0a7d1f623ce3fc8118c4235d2694e03cceea8db756ec8f67e3a346
anchor-sha256: src/plan/format/xr_xtp_artifact.c ed8328a99f27b5bbed4b0a0909f0e42c67ebfff066e80e1bdd4ea01439ebf9d1
anchor-sha256: src/plan/format/xr_xtp_decode.c 385d185d9a86fcc01d1bea045ccc79692bac3e826e2c14145615721085e4bf8e
anchor-sha256: src/plan/format/xr_xtp_rows.c 85e8842a3857fd250c68c5cc12b7aba35787461650317dacdb39eaf92da317a9
anchor-sha256: src/runtime/abi/xr_target_machine_facts.h 8c8d1c341fb4639bb47c982ac6dfd851571d154823101e00011a63fcb14486d8
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.h 5ea9aa4ff63d88b62dbf1f43bea9ab2875d9d63ef2722d73df5a71c59eedda1b
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c e5202e6650d78162c338bbd276150447ef853a662fa253cc977ea64141d5b247
anchor-sha256: src/plan/target/xr_target_profile.h ec795265cca3a1ccbe82cdd1dda1387ec89a27bdb97247678238d72dd54e85e3
anchor-sha256: src/plan/target/xr_target_profile.c aa5b7db6be962e0deaedd2de4ae105f87f777d392fbf30e72b4da8704efa248f
anchor-sha256: src/plan/target/xr_target_plan.h 9cc8176a48d405c78ddfecf8c4a86173b21ee34ad5fa83e0ccbf04da1161b6c4
anchor-sha256: src/plan/target/xr_target_plan.c 2080e5263d00cbc2777c413c6db64b177bb90bc56a1629cbbebea1e94de0b2b4
anchor-sha256: src/plan/target/xr_target_builder.c fb8da60ddff4da393c3bafb4161316696d782bf1fbb9b36f8b036dfb2e9f3ecb
anchor-sha256: src/plan/target/xr_target_verify.c fafb0166c2469911739d57894bb499315488a0f3aac5a064b759aa8f6489b2d3
anchor-sha256: src/plan/target/xr_xtp_materialize.c c43b819e15e02784e50911b46ad3ec228b2069c114a7691216abbf59ab6bcdbf
anchor-sha256: src/runtime/xr_runtime_artifact_authority_internal.h 9bf3dbbd4ad323ee8a7745fce137c49b3a50992dc9a443c1851d95ec8a048e2b
anchor-sha256: src/runtime/xr_runtime_artifact_authority.c eca95f69c7cf1e562ddd50b39787f7fe6e842c9e99f04baf72419854060ad317
anchor-sha256: src/runtime/xr_runtime_artifact_verify.c aa42e2ea69d8e2669f1019905a213c62002e9c0d07d4e5df14e0758d8fc14c4a
anchor-sha256: src/runtime/xr_target_plan_load.c 274c5ce32727b7c6bf6d956d65c2f6b10586c39752adce3e47aca9583824de19
anchor-sha256: src/app/cli/xcmd_run.c 804778d75ab7c3e806dc9258637f96d9c96786c07d742a6cd115afefa3bf8488
anchor-sha256: contracts/target-machine/legacy-product-residue.json 052cac030a7f91c22c7c4a40e2571f3d9f0a274661aa1396a13f5c5bf4600827
anchor-sha256: scripts/check_legacy_product_residue.py 0388d636da6384ea62bfaf8401764955541be24b207511727c33af2d85f3a11f
anchor-sha256: tests/unit/plan/test_target_plan.c d71b938b995341540dc3b87e5ec24caf54784cd91db0d09f5c713a785e1ff04c
anchor-sha256: tests/unit/plan/test_xtp_format.c ea7a494431d2d26185a649ed9b6641daa1ac07a92d69c6f538e3c57245597329
anchor-sha256: tests/unit/CMakeLists.txt 23c9e8a7fdac7c760c712bc08dcc5a88651ced5eb5910261583c8bc3440cf383
anchor-sha256: tests/unit/runtime/test_runtime_target_plan_load_archive.c 2f1c3edbdb31af5e60f63d8333bad6685d4b415b09f02c03a89f11e816f6bbfb
anchor-sha256: tests/cli/run_target_artifact_boundary_tests.py 1dcd536216bc9bfbeb45c5f31fbb8a99f39b1ecae2923abf60aefbb7cfd16d21
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 3ec8c875fcabbaeb42cc83a4a20782b3b2edab52a7912362215483a40da241e6
