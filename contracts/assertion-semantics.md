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
anchor-sha256: src/shared/xr_deep_equality.h c75050b915deb7d10d24b5a0163bc454267aafda9a474ae7d1f2a769b78662a8
anchor-sha256: src/shared/xr_core_intrinsic_registry.c de9f899f135bbf6281435d13b3aa2035cdfd2702d6806ad56de823844aae2c20
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c 9307147e728fe4048079ae1fce4f70050d2f1a64c1e84b3ef5e6cf6391f8b09c
anchor-sha256: src/ir/xi_emit_eh.c 7a4ce34a6ba0d1af68d904274ec6a32f792bb4f8144dbc73f66f1e6fc1371aec
anchor-sha256: src/ir/xi_verify.c 8b7a0bbac6a878f550d2837823a3f486324a57123523fae6d1a7394a10ab4b7d
anchor-sha256: src/vm/xvm_dispatch_assert.inc.c 585f98a7252654dbeee7d817837180b0dcdc2b1b77623834d1f8a31d9427ea0a
anchor-sha256: src/aot/xrt_assertion.h 5bfb7083ad7d78343557366424919a99403fc8e3a64ed51903c7d2860c46095d
anchor-sha256: src/aot/xrt_core_freestanding.h d37e499639eb83c3f6b43d29085b719105b79bc259d156eb601fcd90eb1f558f
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 305b629dadd7341284c17ea362629f7e2fc1e2ea7abe3d710c30f0131ba9609a
anchor-sha256: src/plan/target/xr_target_capability.h dcb47fdd35a5ded48e3ddf7d7b855f06579810734730dc5b2108257110847c0a
anchor-sha256: src/plan/target/xr_target_profile.h 81f680104fea0f2782064f8bb39d3a0cf3384a222df9b5636ee33bdfd89cb1a0
anchor-sha256: src/plan/target/xr_target_builder.c 0b0dc738b36c4d663fd587470b290f89b8b61005ff041ed03bffa2608f2e6582
anchor-sha256: src/plan/target/xr_target_verify.c 7edb42d7838433dd8593f747c93f6638b5fe9c4eb2480cfa7dd1ed1e17a946aa
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c d0836e34b01e009e2a260b4c4c022b6a7f7a4e9ce80e77f18fc23b2d0e912ce2
anchor-sha256: tests/unit/aot/test_xrt_assertion.c 5e03408d6b96adfdc657951ba18d3edcf3a788b3609ef2a0151362d0847416bf
anchor-sha256: tests/unit/aot/test_xrt_assertion_freestanding.c 44d79358e0b620abaa43e3ed37830ebdc3787c5bb979bd526cdaa647a788501d
anchor-sha256: tests/aot/run_freestanding_assertion_provider_test.py ed01a22dcc10d26dfbe65bf9b99c55ab9db733e5742f2337b9a160dcfabd9a4a
anchor-sha256: tests/unit/ir/test_xi_emit.c a809e6a15186b3e46190591fb5a90ae19f5ab76ec229c8984c61c608d7f37a85
anchor-sha256: tests/unit/plan/test_target_profile.c f23dff5f2931febf86cbe5b6a5e4c69db54bce463a01db6f782ded3ffcd4ed1c
anchor-sha256: tests/unit/plan/test_target_plan.c 16a70e037b6e52b9182ac56efbfdb590e388d7098033883e16a42514222ce76e
anchor-sha256: tests/unit/fixtures/assertion/same_t_contextual.xr 865f66dda824e04543a140d5423855c6199b60b13673c292b7ee990c58a550fa
