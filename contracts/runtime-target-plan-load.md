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
   String-literal storage, and direct-local-callee storage. The
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
   Foundation capability masks are also exact. Allocator and panic
   requirements have dense records bound to the same canonical provider kinds.
   Missing, duplicate, additional, or mismatched family,
   capability, or provider records fail before any activation boundary.
5. Runtime loading accepts only an XTP v10 match, decodes a bounded candidate,
   binds its identity to the authority, materializes typed rows, and invokes
   independent TargetPlan verification. V10 is a breaking hard cutover from
   v9. It preserves closure-storage, coroutine-state-call, TargetProfile v2
   String-literal authority, String-literal-storage completion, and the sealed
   `Channel.close()` descriptor while adding the exact direct-local-callee-
   storage family described above. V9 is rejected rather than reinterpreted.
   The `Channel.close()` receiver is a dispatch target rather than an argument
   or slot, and its descriptor grants no general method ABI or execution
   authority. V10 also
   includes the optional canonical instruction table in the exact digest and
   typed-row codec. A function with no instruction rows has no execution
   authority; loading it is not an empty successful program. The loader
   returns only an immutable verified TargetPlan and performs no callback,
   provider lookup, registration, or execution.
6. Installed runtimes construct artifact authority only from exact XSM bytes.
   XSM decoding applies the current schema, operation-registry fingerprint,
   payload digest, ownership replay, semantic fingerprint, and independent
   SemanticPlan verifier before the runtime builds its canonical native
   TargetProfile. They do not guess SemanticPlan or TargetProfile authority
   from a host name, file name, sibling artifact, or caller-authored
   fingerprint.
7. The installed positive route is deliberately limited to exact XSM plus its
   matching XTP and the sole-function scalar generation executor. A build-time
   fixture generator freezes exact bytes for the current native profile; the
   executed archive test links only the installed runtime archive, constructs
   authority from XSM, materializes the verified XTP, prepares and activates
   one generation, and executes function 0 to its exact scalar result. No XSM
   or XTP encoder enters the runtime archive. Plans with exports, calls, roots,
   storage, allocation, adapters, or coroutine execution authority still fail
   PREPARE; this route is not general module activation.
8. Decoder and materializer diagnostics retain their governed stage code at
   the public API and CLI boundaries. They are not folded into a generic
   schema or unverified-artifact error.
9. Legacy product residue is a monotonic ceiling: complete owner removal and
   terminal zero are valid, while new owners, tokens, symbol names, or count
   growth fail. The installed archive gate fails when inspection is
   unavailable, rejects compiler and `xi_` symbols, and treats any new `xvm_`
   symbol as uninventoried legacy residue.

anchor-sha256: CMakeLists.txt c3c9d9c6d90e8f449f169a907438f2b12e3eb6baab2cf965a6fcf1c0f504a8b3
anchor-sha256: include/xray_target_plan_load.h b4908c5917da540471ca4093eacd3dc231f465362d0f126da24700d0404def42
anchor-sha256: src/plan/format/xr_artifact_kind.h 38fd73865e25d62392d8dd0abfd5e6193edf2356803a817349db14c43ccf9874
anchor-sha256: src/plan/format/xr_artifact_kind.c 289fb506284ed97372e211225dcb5c1d205d7149416ddc96abb2a7f3b8704b39
anchor-sha256: src/plan/format/xr_xsm_schema.h 88dfd41f4086139d40f40cc991861137ef5020f5a428ea1fb0ba99a7adca05b0
anchor-sha256: src/plan/format/xr_xsm_decode.c d54abb3c877ed3127b1ffe33642270fd3ec8cb2958cf46dfaf9b5de9a3c21007
anchor-sha256: src/plan/format/xr_xtp_schema.h 257b840e064875dde48ace6dd593b9ae98f533df0994983996d0d11a1ee13f72
anchor-sha256: src/plan/format/xr_xtp_internal.h a07aa91f18d9078e0f80adce2e49629eff4ac9825daf23c26f9c79889e354548
anchor-sha256: src/plan/format/xr_xtp_artifact.c ed8328a99f27b5bbed4b0a0909f0e42c67ebfff066e80e1bdd4ea01439ebf9d1
anchor-sha256: src/plan/format/xr_xtp_decode.c 385d185d9a86fcc01d1bea045ccc79692bac3e826e2c14145615721085e4bf8e
anchor-sha256: src/plan/format/xr_xtp_rows.c 4a443bd20e20e63eb9064bb6e81cebb55bcd218abd4a10990b9a689af4a128b8
anchor-sha256: src/runtime/abi/xr_target_machine_facts.h 8c8d1c341fb4639bb47c982ac6dfd851571d154823101e00011a63fcb14486d8
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.h 5ea9aa4ff63d88b62dbf1f43bea9ab2875d9d63ef2722d73df5a71c59eedda1b
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c e5202e6650d78162c338bbd276150447ef853a662fa253cc977ea64141d5b247
anchor-sha256: src/plan/target/xr_target_profile.h 629a0a7807a698aecb1c8dbc660fa975359ae4050760cba639c259a10ff32530
anchor-sha256: src/plan/target/xr_target_profile.c aa5b7db6be962e0deaedd2de4ae105f87f777d392fbf30e72b4da8704efa248f
anchor-sha256: src/plan/target/xr_target_plan.h fcb5361be828a1a0164c9ba6d01f0df7c9444f6f5088b59025158a8f2c66ad3d
anchor-sha256: src/plan/target/xr_target_plan.c b6bf2d627ceac8ea3b4878bfa543ac8f131e323c916af4bd7edce5bec28537f2
anchor-sha256: src/plan/target/xr_target_builder.c 532dde8cee908bee915dc137b55cafe93a4b910f9f007c2994772c107fe3a1c8
anchor-sha256: src/plan/target/xr_target_verify.c 2997198df1c8e8886f99674e04c677d0e37901fcc755a9bf2e289cc9b0c832e0
anchor-sha256: src/plan/target/xr_xtp_materialize.c b8196ae0744dfa56b23c1a0f93e27aabf761340b0b3ad28c45e952b32b69ebe7
anchor-sha256: src/runtime/xr_runtime_artifact_authority_internal.h 9bf3dbbd4ad323ee8a7745fce137c49b3a50992dc9a443c1851d95ec8a048e2b
anchor-sha256: src/runtime/xr_runtime_artifact_authority.c eca95f69c7cf1e562ddd50b39787f7fe6e842c9e99f04baf72419854060ad317
anchor-sha256: src/runtime/xr_runtime_artifact_verify.c aa42e2ea69d8e2669f1019905a213c62002e9c0d07d4e5df14e0758d8fc14c4a
anchor-sha256: src/runtime/xr_target_plan_load.c f13d9c78eb71a83378528966efb420d84084406e92ba8b0a0c1a9327bd755c03
anchor-sha256: src/app/cli/xcmd_run.c 1fea33de365a7c19a4442b04927101655e68b9467b5fb7016756d3055b51a91c
anchor-sha256: contracts/target-machine/legacy-product-residue.json 052cac030a7f91c22c7c4a40e2571f3d9f0a274661aa1396a13f5c5bf4600827
anchor-sha256: scripts/check_legacy_product_residue.py 0388d636da6384ea62bfaf8401764955541be24b207511727c33af2d85f3a11f
anchor-sha256: tests/unit/plan/test_target_plan.c 83051a58584a6a9a25e2aaf3bf94b930fe5afec718f4559fe78c8b95e684c9d0
anchor-sha256: tests/unit/plan/test_xtp_format.c f97746e16b612fa047054ff26f3d1ba0a47597b48c04cf061ebf1233d78a2de4
anchor-sha256: tests/unit/CMakeLists.txt 3b70eacbac8d8c3001e6020c12c30103d55d630830949e0ab40af5ef18a8524a
anchor-sha256: tests/unit/runtime/test_runtime_target_plan_load_archive.c 2f1c3edbdb31af5e60f63d8333bad6685d4b415b09f02c03a89f11e816f6bbfb
anchor-sha256: tests/cli/run_target_artifact_boundary_tests.py 25b4982fdba5605758f7351c4220ff7a78505920916cf85610cd70b06bb7838b
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 3ec8c875fcabbaeb42cc83a4a20782b3b2edab52a7912362215483a40da241e6
