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
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c 6ff80ebb347cdb101415c149121dbc991cbb3d9d147b3e8f082460167b6b27e1
anchor-sha256: src/ir/xi_emit_eh.c 9d897a33638af29e59487dabea805834fb73845e8aa2503c2cbed62ca402af91
anchor-sha256: src/ir/xi_verify.c eba9de544590ee8a28c1998bc72ad0d78eedc3dfc8d487e0e875ba549922ad77
anchor-sha256: src/vm/xvm_dispatch_assert.inc.c bd5ba9f3e4f36716cbfa8a28344566a24776756fbfff415b2e759316bc34bc7c
anchor-sha256: src/aot/xrt_assertion.h 5bfb7083ad7d78343557366424919a99403fc8e3a64ed51903c7d2860c46095d
anchor-sha256: src/aot/xrt_core_freestanding.h 5ea90e48514a3bbaa483634bfa58dd0f4c9566d790c284ecc337b9bef274c55f
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 7cede8bd2f4387becdcc418822ab5c54aad3573ec3262dad56363b5feae11f41
anchor-sha256: src/plan/target/xr_target_capability.h 5069e52a258729e7c1384d1cea8706aba2cfa639d45ad94280bda0d89bd7a3e0
anchor-sha256: src/plan/target/xr_target_profile.h b618595d2c175e4a6964093f8161bafd79c9ba1860992ff8952cf6e9229e1c8d
anchor-sha256: src/plan/target/xr_target_builder.c 9636604df6825452c6b1d438976a88a7003d2bb1d67d4776be8f720f5badb793
anchor-sha256: src/plan/target/xr_target_verify.c 4dccbf52e4e59a6a7b9ca34c2eb28a933737519d7aa72ea3312fad93d79cc86f
anchor-sha256: src/runtime/abi/xr_runtime_target_authority.c 9dbcd9bb6c45929afa99c64cb4dedafcdb1dcb9011344e33fb82c312adfffadb
anchor-sha256: tests/unit/aot/test_xrt_assertion.c 5e03408d6b96adfdc657951ba18d3edcf3a788b3609ef2a0151362d0847416bf
anchor-sha256: tests/unit/aot/test_xrt_assertion_freestanding.c 44d79358e0b620abaa43e3ed37830ebdc3787c5bb979bd526cdaa647a788501d
anchor-sha256: tests/aot/run_freestanding_assertion_provider_test.py ed01a22dcc10d26dfbe65bf9b99c55ab9db733e5742f2337b9a160dcfabd9a4a
anchor-sha256: tests/unit/ir/test_xi_emit.c ccf5b9b940c2badf4504533cb5debeaebcb9b5249439b20ff5ea3279713d3458
anchor-sha256: tests/unit/plan/test_target_profile.c ebcd1c0fef635f5e4997fd41523f47349b5c6ccaf868eaa491789993abaab8ac
anchor-sha256: tests/unit/plan/test_target_plan.c a8156085c669aae357519bdf5296694c2df39358bd8f6625c5fea6f014e482ab
anchor-sha256: tests/unit/fixtures/assertion/same_t_contextual.xr 865f66dda824e04543a140d5423855c6199b60b13673c292b7ee990c58a550fa
