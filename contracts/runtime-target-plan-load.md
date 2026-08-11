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
   SemanticPlan and exact TargetProfile. The native runtime owns and rebuilds
   the canonical architecture, operating system, environment, native ABI,
   complete data layout, atomic widths/orders, float features, vector
   features/width, and runtime profile. Both authority construction and the
   independent verifier compare every field against that runtime-owned
   snapshot before accepting the TargetProfile; caller-authored profile or
   runtime/provider/object fingerprints cannot authorize a foreign machine.
   The current runtime has no governed SIMD discovery or installed SIMD
   manifest, so its exact native execution authority is scalar-only. Any
   nonzero vector feature or maximum vector width fails closed.
4. The required TargetPlan family and foundation capability masks are exact.
   Allocator and panic requirements have dense records bound to the same
   canonical provider kinds. Missing, duplicate, additional, or mismatched
   capability/provider records fail before any activation boundary.
5. Runtime loading accepts only an XTP v3 match, decodes a bounded candidate,
   binds its identity to the authority, materializes typed rows, and invokes
   independent TargetPlan verification. It returns only an immutable verified
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
anchor-sha256: src/plan/format/xr_xtp_schema.h f6bb0d24df9a324f272cc469fefc44838fbc25596ec84527e49b39a23bc4e5d2
anchor-sha256: src/plan/format/xr_xtp_internal.h a07aa91f18d9078e0f80adce2e49629eff4ac9825daf23c26f9c79889e354548
anchor-sha256: src/plan/format/xr_xtp_artifact.c ed8328a99f27b5bbed4b0a0909f0e42c67ebfff066e80e1bdd4ea01439ebf9d1
anchor-sha256: src/plan/format/xr_xtp_decode.c 385d185d9a86fcc01d1bea045ccc79692bac3e826e2c14145615721085e4bf8e
anchor-sha256: src/plan/format/xr_xtp_rows.c 634a9bc1e6454894f71ca13c62daa4fdba867dad9519f96a416efb6dc3925481
anchor-sha256: src/runtime/abi/xr_target_machine_facts.h 8c8d1c341fb4639bb47c982ac6dfd851571d154823101e00011a63fcb14486d8
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.h 40074f57122cd244cbb8b1c7da46f7ea415df76c2279b84d7fd7d5e276cedf5f
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c 2cf42c3a46e6d78b67509f9a701ad9ab2cc088ac3d69dabf538aba3450d3f853
anchor-sha256: src/plan/target/xr_target_profile.h afdf3e381e5b148b6c229086f5726abdbb8811029448e3646b1c20d18eb18f03
anchor-sha256: src/plan/target/xr_target_profile.c 170fd341ba7af1f1e2fc3ce89184c729539b6eca8dba751a7b68b3c5f78a1909
anchor-sha256: src/plan/target/xr_target_plan.h 593bfc760b028a5d0a1afbc0296084d6e42d67257dcc954b84de5f2d852c1104
anchor-sha256: src/plan/target/xr_target_plan.c a971b6149df79ae9865ea90388b4be2cc1778c47cc107a2a4eabc66fcef1c6b6
anchor-sha256: src/plan/target/xr_target_builder.c 49d8f873b0430eff491563a8b37834134bd62bdff86017475f04ab6ca7372584
anchor-sha256: src/plan/target/xr_target_verify.c 7854c0e0c2473de0e98565366ac80b9179d38355dd2d7fe8f1c8e28f3fb971e1
anchor-sha256: src/plan/target/xr_xtp_materialize.c dd6987e48b19ec0bfd93fed4b7362c83a93c09aaf01242aa42ca5dec7738d21d
anchor-sha256: src/runtime/xr_runtime_artifact_authority_internal.h 151a9f428bdf7654668cb062900b34781c8ef684150e0880eeb8658e6885f040
anchor-sha256: src/runtime/xr_runtime_artifact_authority.c 20c9060268e452f34bb86f95482937ef69975d278532df7491a40ed6b3c693b0
anchor-sha256: src/runtime/xr_runtime_artifact_verify.c aa42e2ea69d8e2669f1019905a213c62002e9c0d07d4e5df14e0758d8fc14c4a
anchor-sha256: src/runtime/xr_target_plan_load.c 481c35a259480f4f11a6f3fec1fadf9a49038749e14a0f657e24bca369ae67ac
anchor-sha256: src/app/cli/xcmd_run.c 868f60ad51f4994a89c8deb27b167dca9800f74174b6fffd28a5203e849585c6
anchor-sha256: contracts/target-machine/legacy-product-residue.json 052cac030a7f91c22c7c4a40e2571f3d9f0a274661aa1396a13f5c5bf4600827
anchor-sha256: scripts/check_legacy_product_residue.py 0388d636da6384ea62bfaf8401764955541be24b207511727c33af2d85f3a11f
anchor-sha256: tests/unit/plan/test_target_plan.c d86fbabe1f8cb2ca78090ab7a7a6412c7a1df3eef59e239491c63898eda84611
anchor-sha256: tests/unit/plan/test_xtp_format.c 9730c18d97a337579d78f1805451d771901528962035cba8b6434ab60c764dfd
anchor-sha256: tests/unit/runtime/test_runtime_target_plan_load_archive.c 0bbbff9cf45d6a41fa8081f1989d8f9deacbc0b5c8a720a69518a6eab8979c3f
anchor-sha256: tests/cli/run_target_artifact_boundary_tests.py 8f8550130d555a0c143e4dcbef6e49881a75705bd41d47bf053c597f53c42293
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 0fee8f623d69002b2daaa6f2fdf99ce315bf85b0b0955caf92aeb8f9982684e0
