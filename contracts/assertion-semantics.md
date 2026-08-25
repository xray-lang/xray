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

anchor-sha256: src/shared/xr_assertion_plan.h e6d5540b6da9c793b83c7372b342154110deb785c03ae07132ff6ed412807f68
anchor-sha256: src/shared/xr_assertion_core.h d5544454487ab0786800ed53256e53a95055f5e12626fd81d1248a34254860b0
anchor-sha256: src/shared/xr_deep_equality.h 0d54b309bd016530cae808ee8e937c5b17e68f8b07fe771effc42debc4f4f8e9
anchor-sha256: src/frontend/analyzer/xa_core_intrinsic_registry.c 29aa339fed9a94072d5bbe72c5320c7f8b0496de70349faaebc9984e4fba814c
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c d1c86a031eb0f9fdbbbacb2680e25bb9953b1dac3f50549035a9d1a01a8279a6
anchor-sha256: src/ir/xi_emit_eh.c 7a4ce34a6ba0d1af68d904274ec6a32f792bb4f8144dbc73f66f1e6fc1371aec
anchor-sha256: src/ir/xi_verify.c b28fb322781a108b43d7dc56ce7f3ce1323f6bd0a7c6f94e0dcc254741362658
anchor-sha256: src/vm/xvm_dispatch_assert.inc.c 585f98a7252654dbeee7d817837180b0dcdc2b1b77623834d1f8a31d9427ea0a
anchor-sha256: src/aot/xrt_assertion.h 5bfb7083ad7d78343557366424919a99403fc8e3a64ed51903c7d2860c46095d
anchor-sha256: src/aot/xrt_core_freestanding.h 5d4e9b2da067c44aa23d0b46b0ae133abeaae6e1b49a8efee617b384cb45cfb6
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c f03d08e15df36ea6ec1160b94307fdc6aa731a2c34f2de1421ee40db1dab5e05
anchor-sha256: src/plan/target/xr_target_capability.h dcb47fdd35a5ded48e3ddf7d7b855f06579810734730dc5b2108257110847c0a
anchor-sha256: src/plan/target/xr_target_profile.h 2e37c59739b459a24a1ec455fbbb84e520579f0f69c67fbbdd995d47d8e3f946
anchor-sha256: src/plan/target/xr_target_builder.c 94aad1858c8813c8183cdc5a69e0a896deb804e1870161c242cecb894be64da6
anchor-sha256: src/plan/target/xr_target_verify.c 90b44fa2b4f8839550e722a8679c6dddf50eeb5801c41a4f85b5a8c595d7d385
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c d0836e34b01e009e2a260b4c4c022b6a7f7a4e9ce80e77f18fc23b2d0e912ce2
anchor-sha256: tests/unit/aot/test_xrt_assertion.c 5e03408d6b96adfdc657951ba18d3edcf3a788b3609ef2a0151362d0847416bf
anchor-sha256: tests/unit/aot/test_xrt_assertion_freestanding.c 44d79358e0b620abaa43e3ed37830ebdc3787c5bb979bd526cdaa647a788501d
anchor-sha256: tests/aot/run_freestanding_assertion_provider_test.py 97b2433d3a7cfe1f2584ad6977b56505c551c4249d057d5dc0953bfdf14cc611
anchor-sha256: tests/unit/ir/test_xi_emit.c 21b97ce449db683e1c26bc07a7d51d8647998678cb087adc61249ba10934b4f7
anchor-sha256: tests/unit/plan/test_target_profile.c f23dff5f2931febf86cbe5b6a5e4c69db54bce463a01db6f782ded3ffcd4ed1c
anchor-sha256: tests/unit/plan/test_target_plan.c 895f8524a5fdbed5a9408e7edf2822ac38b203f5e99dce7bad857c21ef4e03b8
anchor-sha256: tests/unit/fixtures/assertion/same_t_contextual.xr 865f66dda824e04543a140d5423855c6199b60b13673c292b7ee990c58a550fa
