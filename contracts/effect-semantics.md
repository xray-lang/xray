# Effect and assertion semantics contract

Status: frozen by task 220.

1. Function throw effect is the tri-state internal dimension `NO_THROW`,
   `MAY_THROW`, or `POLY`. Incomplete evidence is `MAY_THROW` (fail-closed).
2. `NO_THROW` is assignable where `MAY_THROW` is accepted; the reverse is
   rejected. `POLY` is an effect variable for callback-parameter positions and
   is specialized to a closed effect at actual call sites.
   An unannotated variable or field that stores a function value is a merge
   boundary and therefore receives an independent `MAY_THROW` function type;
   it must not inherit a precise `NO_THROW` bit from its first initializer.
3. Error sets remain outside function types. The throw bit is derived from the
   canonical effect summary and is consumed constructively by Xi lowering.
4. `@no_throw`, `@no_suspend`, `@no_alloc`, and `@zero_cost` are assertions,
   never optimization hints. An unproved assertion is rejected fail-closed.
5. On a directly inferred no-throw function, adding `@no_throw` changes which
   future edits are legal but produces byte-identical generated C. Equivalent
   assertion invariants apply to the other `@no_*` contracts.
6. Changing defaults, subtype direction, incompleteness handling, assertion
   failure behavior, or the equivalence invariant is a contract change.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_assertion_attr.def 82cf5e56e8fedd3bc9ea6f824ef6ccf86534dbfb5934291cae1a15bc75652f54
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c c32fa97e58b2acf2548bf336b3428c46dd2a3e5cc4a41d8e5667dcfa969885e5
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h 550f1db5cd68b81fa1c72c33c47e3d9a1d047e7b4ff07b132ffe8a2705d3c880
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c 0fdcd8ac3405167f28baeabda736d4aebaad9c1e52a75e2f8f1f1aa5fd2742b2
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c b6bbfb857bdffd92de8ed5a47c4520cfa0d301d28ae763c12c4b17ee3b6f4448
anchor-sha256: src/runtime/value/xtype.h f4c2d04ab84ca7a53132717fa67716432e61a46bcbd5cdd59a12e8212d95ed7c
anchor-sha256: tests/aot/run_no_throw_contract_tests.sh df115e95a79d8101345c574e025fcf629208c8a6e00ae4415b1333b622de0d4a
anchor-sha256: tests/unit/analyzer/test_analyzer.c ddf2fa1edd47fd550ebd02505f539203b62b0ba29aef94047359d4a9b7e06bbb
