# Runtime TargetPlan artifact load contract

This contract freezes the non-executing runtime boundary for TargetPlan
artifacts. It does not claim module activation, provider registration, an entry
cell, generation ownership, finalization, or executable code.

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
   features/width, runtime profile, ABI, object header, and provider set before
   building the profile. The independent verifier re-derives the same fields;
   caller-authored profile or runtime/provider/object fingerprints cannot
   authorize a foreign machine or a foreign XTP artifact.
   The current runtime has no governed SIMD discovery or installed SIMD
   manifest, so its exact native execution authority is scalar-only. Any
   nonzero vector feature or maximum vector width fails closed.
4. The required TargetPlan family and foundation capability masks are exact.
   Allocator and panic requirements have dense records bound to the same
   canonical provider kinds. Missing, duplicate, additional, or mismatched
   capability/provider records fail before any activation boundary.
5. Runtime loading accepts only an XTP v5 match, decodes a bounded candidate,
   binds its identity to the authority, materializes typed rows, and invokes
   independent TargetPlan verification. V5 includes the optional canonical
   instruction table in the exact digest and typed-row codec. A function with
   no instruction rows has no execution authority; loading it is not an empty
   successful program. The loader returns only an immutable verified
   TargetPlan and performs no callback, provider lookup, registration, or
   execution.
6. Installed runtimes explicitly report that authority artifact loading is not
   available. They do not guess SemanticPlan or TargetProfile authority from a
   host name, file name, sibling artifact, or caller-authored fingerprint.
7. Decoder and materializer diagnostics retain their governed stage code at
   the public API and CLI boundaries. They are not folded into a generic
   schema or unverified-artifact error.
8. Legacy product residue is a monotonic ceiling: complete owner removal and
   terminal zero are valid, while new owners, tokens, symbol names, or count
   growth fail. The installed archive gate fails when inspection is
   unavailable, rejects compiler and `xi_` symbols, and treats any new `xvm_`
   symbol as uninventoried legacy residue.

anchor-sha256: include/xray_target_plan_load.h 93f54196db1860947dca8dfe410e587e1676e8b9a24649fb8be7ad28c5a5f74f
anchor-sha256: src/plan/format/xr_artifact_kind.h 38fd73865e25d62392d8dd0abfd5e6193edf2356803a817349db14c43ccf9874
anchor-sha256: src/plan/format/xr_artifact_kind.c 289fb506284ed97372e211225dcb5c1d205d7149416ddc96abb2a7f3b8704b39
anchor-sha256: src/plan/format/xr_xtp_schema.h cf4a6ddb9385ef378dbea1b9e76e6780e831c026aaa4db5c7f78bf0e99243944
anchor-sha256: src/plan/format/xr_xtp_internal.h a07aa91f18d9078e0f80adce2e49629eff4ac9825daf23c26f9c79889e354548
anchor-sha256: src/plan/format/xr_xtp_artifact.c ed8328a99f27b5bbed4b0a0909f0e42c67ebfff066e80e1bdd4ea01439ebf9d1
anchor-sha256: src/plan/format/xr_xtp_decode.c 385d185d9a86fcc01d1bea045ccc79692bac3e826e2c14145615721085e4bf8e
anchor-sha256: src/plan/format/xr_xtp_rows.c 2354fa4354931519ecf8093b27d2ac1107b47fc9691ccf5007d9db381f8fa384
anchor-sha256: src/runtime/abi/xr_target_machine_facts.h 8c8d1c341fb4639bb47c982ac6dfd851571d154823101e00011a63fcb14486d8
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.h 40074f57122cd244cbb8b1c7da46f7ea415df76c2279b84d7fd7d5e276cedf5f
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c 2cf42c3a46e6d78b67509f9a701ad9ab2cc088ac3d69dabf538aba3450d3f853
anchor-sha256: src/plan/target/xr_target_profile.h f42aa32a7c41b113adf4d7ab877e10260cdc4c187472cb19d5cfa54bd20d7aa6
anchor-sha256: src/plan/target/xr_target_profile.c 170fd341ba7af1f1e2fc3ce89184c729539b6eca8dba751a7b68b3c5f78a1909
anchor-sha256: src/plan/target/xr_target_plan.h 966d08684c5c4db79ecd86180d6a5c590bb23a7ae5dd364e87807836bd7278e4
anchor-sha256: src/plan/target/xr_target_plan.c 3a2223d2bc7cf0e9b8152a5a79daea0c79fb9007bafc85b6c1751332f88ae051
anchor-sha256: src/plan/target/xr_target_builder.c 66ff1abed78e1bbe9ba1a351554d5f965c7fe28d2375209b3536dfc3dbf43ae2
anchor-sha256: src/plan/target/xr_target_verify.c 19c858372137e59b1eee385ea0ef1e5923e3ad0df2eb99cd1e793eb746a01084
anchor-sha256: src/plan/target/xr_xtp_materialize.c b8196ae0744dfa56b23c1a0f93e27aabf761340b0b3ad28c45e952b32b69ebe7
anchor-sha256: src/runtime/xr_runtime_artifact_authority_internal.h 9bf3dbbd4ad323ee8a7745fce137c49b3a50992dc9a443c1851d95ec8a048e2b
anchor-sha256: src/runtime/xr_runtime_artifact_authority.c 2a04c9634794e1b63e0b168389cf04f2e57b8a98c2b3cc3ac6cf228043599c5a
anchor-sha256: src/runtime/xr_runtime_artifact_verify.c aa42e2ea69d8e2669f1019905a213c62002e9c0d07d4e5df14e0758d8fc14c4a
anchor-sha256: src/runtime/xr_target_plan_load.c fd354903a661e136a5e5f5a408ce90deb1792d3f5043d7f5979a9e1b0b061bd5
anchor-sha256: src/app/cli/xcmd_run.c aeee2c270da95da4257f1755d7bb8e612d8fb5aa33815486262a5a35625788b1
anchor-sha256: contracts/target-machine/legacy-product-residue.json 052cac030a7f91c22c7c4a40e2571f3d9f0a274661aa1396a13f5c5bf4600827
anchor-sha256: scripts/check_legacy_product_residue.py 0388d636da6384ea62bfaf8401764955541be24b207511727c33af2d85f3a11f
anchor-sha256: tests/unit/plan/test_target_plan.c 3fe7becd273098313be9139d07a1db4afb2866eb5fc04042914afa838186a6a6
anchor-sha256: tests/unit/plan/test_xtp_format.c 0380e403cfa7c993e5893cfae029932596d0e1ac430b32bae6dc0ad0da894d03
anchor-sha256: tests/unit/runtime/test_runtime_target_plan_load_archive.c 0bbbff9cf45d6a41fa8081f1989d8f9deacbc0b5c8a720a69518a6eab8979c3f
anchor-sha256: tests/cli/run_target_artifact_boundary_tests.py 1394a6f9dec194b468b4c57dc4692a39931bd8f947e0b0a2e8f398e9863e3fff
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 0fee8f623d69002b2daaa6f2fdf99ce315bf85b0b0955caf92aeb8f9982684e0
