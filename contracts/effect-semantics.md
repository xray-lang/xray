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
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c 79fe9274fc91debdb9216094cfb1dbec0cb5995b1f0ad469f74bf8948e519f42
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h a2c2860867bbdb8b29e407ec18f20291965131dece31715888468eb041150879
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c b224cbac94a5aaa69dfaa0af5d9387dd88cf8977a5dbb3b488f1697bfdb59c88
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c d35621aca786e478845b66ef885eb3ea127dc0f3cdf2fc51da4405005d144f79
anchor-sha256: src/runtime/value/xtype.h a69879874fb21375346041e84a065fd46f5f59a9eca97356b69e66cb265a41fe
anchor-sha256: tests/aot/run_no_throw_contract_tests.sh df115e95a79d8101345c574e025fcf629208c8a6e00ae4415b1333b622de0d4a
anchor-sha256: tests/unit/analyzer/test_analyzer.c 7ed575511b4c5b95ba36638990b8629d9174d52c30023b3e42ae319e0f151970
