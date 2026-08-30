# Assertion Semantics Contract

This contract freezes the typed source, semantic, Xi, VM, AOT, and target
capability boundary for core assertions.

1. The only compiler-owned source identities are `assert`, `assertEqual`,
   `assertThrows`, and `assertPanics`. Binding and lowering select them through
   stable builtin identities, never through source-name switches.
2. `assertEqual` admits one exact inferred type for both operands. Contextual
   literals and null are resolved symmetrically from the sole non-contextual
   anchor. Analysis may defer a pure first literal, but executable lowering and
   message evaluation preserve source left-to-right order and evaluate each
   expression once.
3. Typed errors and panics are separate observation channels. `assertThrows`
   succeeds only for a typed error and `assertPanics` succeeds only for a panic.
   Normal return, the wrong channel, and simultaneous channels are distinct,
   fail-closed outcomes.
4. VM and AOT consume the same assertion plan, failure schema, bounded renderer,
   and deep-equality kernel. Rendering succeeds only when the complete byte
   sequence fits. Truncation, invalid schema, allocation failure, and exception
   construction failure retain no partial semantic result.
5. Assertion action results and caught values have explicit consume/borrow
   ownership. Every success, failure, panic, conflict, and fault-injection path
   releases owned observations and preserves borrowed operands.
6. Deep equality is tag-strict and type-directed. Aggregate recursion uses a
   shared cycle policy. Map keys and Set members are matched by the canonical
   key-equivalence authority before values are compared recursively.
7. A single `xi.assertion` operation owns a deep-copied assertion plan. Clone,
   verify, dump, hash, edit, and destroy paths must cover the auxiliary payload;
   retired assertion opcodes are not accepted.
8. Target planning derives assertion capabilities from the frozen target
   profile. Freestanding assertion reporting requires the exact IO provider
   identity and operation ABI. Missing or incompatible providers fail before C
   emission; typed-error and unwind boundaries are never inferred from hosted
   behavior.
9. C generation selects the assertion adapter through a stable typed identity.
   A C symbol spelling is a final projection only and cannot select semantics.

## Digest anchors

anchor-sha256: src/shared/xr_assertion_plan.h 53463aef97f4b92b40cb2247b4df921aade9944aa42dbb04d3a47a84c8e9c921
anchor-sha256: src/shared/xr_assertion_core.h d5544454487ab0786800ed53256e53a95055f5e12626fd81d1248a34254860b0
anchor-sha256: src/shared/xr_deep_equality.h c75050b915deb7d10d24b5a0163bc454267aafda9a474ae7d1f2a769b78662a8
anchor-sha256: src/shared/xr_core_intrinsic_registry.c 97197195363f6f21f9eefaa6440373e369ab70569a02aa4fe4afba8cac3ce13c
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c e33606ab608e06c87de7e65a65576b15f4e69f509a15e12504e4fd58f27e49a6
anchor-sha256: src/ir/xi_emit_eh.c 7a4ce34a6ba0d1af68d904274ec6a32f792bb4f8144dbc73f66f1e6fc1371aec
anchor-sha256: src/ir/xi_verify.c 8b7a0bbac6a878f550d2837823a3f486324a57123523fae6d1a7394a10ab4b7d
anchor-sha256: src/vm/xvm_dispatch_assert.inc.c 0f3642f87578f9c60d7e66b1fb6bad0d8a799abb14e3f83086e445e47efb0e3e
anchor-sha256: src/aot/xrt_assertion.h 5bfb7083ad7d78343557366424919a99403fc8e3a64ed51903c7d2860c46095d
anchor-sha256: src/aot/xrt_core_freestanding.h 357372683ceeb4e51f65ea00ae40c7f22f05187ab271368562ab10e0db40e082
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c c1c6bdc62a3cdc9ec9f11dc6851f8d09202befb5a2b3d52a8cafe404690c641c
anchor-sha256: src/plan/target/xr_target_capability.h 5069e52a258729e7c1384d1cea8706aba2cfa639d45ad94280bda0d89bd7a3e0
anchor-sha256: src/plan/target/xr_target_profile.h 4c92d70d5046a4af2363c9036b0ed1a5a44651051f6159933f3e0376cfb18c2c
anchor-sha256: src/plan/target/xr_target_builder.c f41c1ffd03836ecdf1ae442d1260f21425360c37f2ef09b70c3b20f6fbd76020
anchor-sha256: src/plan/target/xr_target_verify.c 637deee6238aef952afa68355d6e2c57eb3bd799750f253742945b2f641f8b7f
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c 9dbcd9bb6c45929afa99c64cb4dedafcdb1dcb9011344e33fb82c312adfffadb
anchor-sha256: tests/unit/aot/test_xrt_assertion.c 5e03408d6b96adfdc657951ba18d3edcf3a788b3609ef2a0151362d0847416bf
anchor-sha256: tests/unit/aot/test_xrt_assertion_freestanding.c 44d79358e0b620abaa43e3ed37830ebdc3787c5bb979bd526cdaa647a788501d
anchor-sha256: tests/aot/run_freestanding_assertion_provider_test.py ed01a22dcc10d26dfbe65bf9b99c55ab9db733e5742f2337b9a160dcfabd9a4a
anchor-sha256: tests/unit/ir/test_xi_emit.c a809e6a15186b3e46190591fb5a90ae19f5ab76ec229c8984c61c608d7f37a85
anchor-sha256: tests/unit/plan/test_target_profile.c f23dff5f2931febf86cbe5b6a5e4c69db54bce463a01db6f782ded3ffcd4ed1c
anchor-sha256: tests/unit/plan/test_target_plan.c 624ad3d28c0cd261fbe88d73e78c0b5941dd166b1dff597b195cc8e95087ac4b
anchor-sha256: tests/unit/fixtures/assertion/same_t_contextual.xr 865f66dda824e04543a140d5423855c6199b60b13673c292b7ee990c58a550fa
