# Program semantic closure identity contract

This contract freezes the target-neutral identity foundation for a closed
program's instantiation graph. It is not a complete program-semantic-closure
implementation: no member or fixed-point summary, export manifest, artifact
codec, Xi, TargetPlan, runtime layout, storage class, call ABI, or aggregate
admission may be inferred from these rows.

1. The program semantic closure has one versioned schema and one mutable
   builder. The builder accepts bounded module, direct dependency, concrete
   type, concrete function, and resolved-call rows. It canonicalizes all rows
   before freezing and cannot be reopened after either verification or failure.
2. Every module row carries an exact stable module identity and complete source
   and export fingerprints. Every dependency row names two present modules and
   a nonzero contract fingerprint. The dependency graph is acyclic and every
   module is reachable from the canonical entry/export root module set.
3. Concrete type identities are domain-separated hashes of the complete policy,
   exact module and declaration, canonical concrete instance identity, semantic
   shape, and ownership fingerprints. Concrete function identities additionally
   bind their canonical concrete instance, complete semantic signature,
   finalized effects, and capability mask. Non-generic instances still require
   an explicit canonical non-generic instance identity. Dense indexes,
   insertion order, target profiles, layouts, and ABI facts never enter these
   identities.
4. A resolved-call row binds the complete policy and one stable callsite to
   exact present caller and callee function identities and a complete
   call-contract fingerprint. A cross-module call requires the matching direct
   dependency row. Entry and exported functions are explicit root kinds. At
   least one root is required, multiple canonical roots are allowed, and every
   concrete function and module is reachable from the root set.
5. Freeze hashes the canonical rows, schema, and complete policy fingerprint.
   `XrGenerationClosureId` is a distinct domain-separated digest of that full
   closure fingerprint. Zero identities, incomplete fingerprints, duplicate
   coordinates, unknown flags, cycles, unreachable rows, and exhausted hard
   budgets fail closed.
6. The verifier is implemented independently from the builder. It reconstructs
   every derived row identity, canonical order, dependency and call closure,
   aggregate fingerprint, and generation identity. A frozen closure with a
   mutated state, row, fingerprint, or generation identity is rejected.
7. This schema has no artifact codec and no executor consumer. Adding a
   serialization format, cache certificate, Xi/SemanticPlan reference, target
   reader, or aggregate ABI decision requires a separately frozen authority and
   independent validation; none may treat this foundation as implicit Target
   admission.

## Digest anchors

anchor-sha256: src/plan/semantic/xr_program_semantic_closure.h 96469d5046d5eeeb6d5d24c71eed77f47a42b291549fd8df2810e4164f979259
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_internal.h 6d4079b9c940eaa12c0e98d5bb0e4c12d0bda9a2e38103c065f1c3ac7a032aa0
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.c 509a0ad8ffd0fdb87bf5ac4456cf41335db7b48bcaf74c645e45b97fb206c9ed
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_verify.c 14d93b8218d406127b36469ba6efc3c36a30c1465fe3302d58e4888341482532
anchor-sha256: tests/unit/plan/test_program_semantic_closure.c f7af9b0276b5ee725f601997ad67cb53a81adbfb712338610d9a030df30c9c2e
anchor-sha256: tests/unit/CMakeLists.txt fbceb3ac18d7a50abaeb8fa48a5837c44d991d1b78484854bd350031cacc39c9
