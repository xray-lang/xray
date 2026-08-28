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
anchor-sha256: src/shared/xr_core_intrinsic_registry.c 11660c702f0e438919444614f4be66f46cc595220711ec9ebd7e93dc7dc026b0
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c 9307147e728fe4048079ae1fce4f70050d2f1a64c1e84b3ef5e6cf6391f8b09c
anchor-sha256: src/ir/xi_emit_eh.c 7a4ce34a6ba0d1af68d904274ec6a32f792bb4f8144dbc73f66f1e6fc1371aec
anchor-sha256: src/ir/xi_verify.c 8b7a0bbac6a878f550d2837823a3f486324a57123523fae6d1a7394a10ab4b7d
anchor-sha256: src/vm/xvm_dispatch_assert.inc.c 43850713b10cf63d4883083ee9f5109879165633cf54307b2d498e299f83fd64
anchor-sha256: src/aot/xrt_assertion.h 5bfb7083ad7d78343557366424919a99403fc8e3a64ed51903c7d2860c46095d
anchor-sha256: src/aot/xrt_core_freestanding.h d37e499639eb83c3f6b43d29085b719105b79bc259d156eb601fcd90eb1f558f
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c b7f3c77e4372ac1b4d677ee4f5fed8e52d61f694143a87864df51ad0fd4c1f6d
anchor-sha256: src/plan/target/xr_target_capability.h 5069e52a258729e7c1384d1cea8706aba2cfa639d45ad94280bda0d89bd7a3e0
anchor-sha256: src/plan/target/xr_target_profile.h 402046722e05c94a26ae05d5ee00f11f17c92da46f367378e8e567dac59c7c31
anchor-sha256: src/plan/target/xr_target_builder.c e997c650ce15f3c4182bdf8e9f406fbdf72e715c8bb565f5221deea7400e80e1
anchor-sha256: src/plan/target/xr_target_verify.c 05e86ee99f352c5d12b1d0ec63b2a0224d746b1d68ae2713985bed15cbfb10b0
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c 9dbcd9bb6c45929afa99c64cb4dedafcdb1dcb9011344e33fb82c312adfffadb
anchor-sha256: tests/unit/aot/test_xrt_assertion.c 5e03408d6b96adfdc657951ba18d3edcf3a788b3609ef2a0151362d0847416bf
anchor-sha256: tests/unit/aot/test_xrt_assertion_freestanding.c 44d79358e0b620abaa43e3ed37830ebdc3787c5bb979bd526cdaa647a788501d
anchor-sha256: tests/aot/run_freestanding_assertion_provider_test.py ed01a22dcc10d26dfbe65bf9b99c55ab9db733e5742f2337b9a160dcfabd9a4a
anchor-sha256: tests/unit/ir/test_xi_emit.c a809e6a15186b3e46190591fb5a90ae19f5ab76ec229c8984c61c608d7f37a85
anchor-sha256: tests/unit/plan/test_target_profile.c f23dff5f2931febf86cbe5b6a5e4c69db54bce463a01db6f782ded3ffcd4ed1c
anchor-sha256: tests/unit/plan/test_target_plan.c 84c4f9677bc6b57184c2820ef933cc12b93664894c32f75ce858f745cb8ffbe1
anchor-sha256: tests/unit/fixtures/assertion/same_t_contextual.xr 865f66dda824e04543a140d5423855c6199b60b13673c292b7ee990c58a550fa
