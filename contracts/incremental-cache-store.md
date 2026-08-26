# Incremental cache store transaction contract

This contract freezes the process-safe storage boundary for recomputable
incremental `.xsm` and `.xtp` artifacts. It does not define dependency-graph
invalidation, compiler-session ownership, or any compatibility reader.

1. Every operation that observes or mutates committed cache state acquires the
   process-shared root lock. Load, rejected-object cleanup, publish, stale-temp
   recovery, collection, and eviction therefore share one serialization
   authority across cooperating processes. The in-process mutex cannot replace
   this operating-system lock.
2. A load owns a copied byte snapshot before releasing the root lock. Artifact
   verification runs after both locks are released, so verifier callbacks may
   reenter the store without creating a lock-order cycle. A rejected snapshot
   is deleted only after reacquiring the root lock and proving that the current
   object has the same encoded size and full SHA-256 digest. A replacement is
   never removed through an ABA path.
3. The configured byte quota charges complete object encodings, including their
   cache headers; fresh crash residues conservatively consume the same storage
   budget until stale recovery removes them. Before an absent key is published,
   collection must prove that live bytes are at most `quota - object_size`.
   Publication cannot rename first and repair an over-quota state afterward.
   An already committed identical key is compared before reservation and cannot
   evict unrelated objects. An object larger than the quota is rejected.
4. Writers create a private temporary file, write and sync the complete encoded
   object, atomically rename it, and sync the containing directory. A crash
   before rename exposes no committed object; a crash residue is removed under
   the root lock. A failed rename or directory sync is an I/O failure, not a
   permissive cache hit.
5. The cache root and artifact-kind directories must remain real directories
   when inspected under the acquired root lock. Symbolic-link and Windows
   reparse-point substitutions fail closed. Object reads reject final-component
   links/reparse points and non-regular files. Cache paths are derived only from
   validated artifact kinds and fixed-width hexadecimal keys.
6. Every successful or failed root-lock release attempts the native unlock and
   unconditionally closes the native handle or descriptor. A native unlock
   error remains an error even though close is still attempted.
7. Cache hits never bypass the caller-supplied independent artifact verifier.
   The verifier and its context are supplied per load or publication and are
   never retained by the store. Parallel module planning can therefore apply
   distinct immutable SemanticPlan/TargetProfile authorities to one shared
   store without mutable verifier rebinding.
   Corrupt, stale, rejected, or unverifiable bytes cannot activate an artifact,
   and there is no legacy cache format, compatibility fallback, or execution
   fallback in this storage boundary.
8. An XTP cache request derives its key from dedicated ProgramSemanticClosure
   and GenerationClosureId fields, the verified SemanticPlan, exact
   TargetProfile, provider and runtime identities owned by that profile, the
   current planner schema and required-family mask, and the requested
   optimization budget. For a dependency-bearing SemanticPlan, its fingerprint
   already binds the ordered dependency module/fingerprint records, and both
   cold build and cache-hit materialization additionally require the exact
   ordered verified dependency SemanticPlan vector. A matching object is still
   only a candidate: the verifier takes an owned XTP snapshot, materializes it
   against those exact authorities, and independently verifies the resulting
   TargetPlan before accepting the hit. The production AOT driver consumes that
   exact owned plan; it does not decode a second time or rebuild semantic rows
   on a hit.
   Wrong keys, authority mismatches, old schemas, corrupt bytes, and valid
   artifacts for another semantic or target identity fail closed to
   recomputation.
9. Parallel TargetPlan workers own disjoint input-indexed result rows. They may
   read immutable SemanticPlan, its exact ordered dependency-plan vector, and
   TargetProfile authorities and use the
   synchronized cache store, but they never install a plan into the compiler
   bundle, emit diagnostics, or update shared statistics. The caller joins all
   workers and then verifies, reports, and installs results strictly in input
   order. Worker completion order therefore cannot change XTP bytes,
   fingerprints, diagnostics, cache counters, or bundle order. Failure is
   reported at the lowest canonical input index after all started workers have
   joined and every independently owned plan is released.
10. Cancellation is a monotonic operation-owned token. Workers check it before
    authority validation, after cache materialization, after planning, and
    after publication. Once requested, the joiner releases every task-owned
    plan and normalizes every row to cancelled before reporting input index
    zero, so timing cannot expose a partial bundle or nondeterministic counters.
    A verified immutable cache object published before the request remains
    available to later operations; cancellation never deletes another
    operation's content-addressed entry.
11. An XSM cache request derives its key from one module summary, and that
    summary is derived only from authorities their owners already verified: the
    module's ProgramSemanticClosure and GenerationClosureId, its verified
    SemanticPlan identity, its ordered dependency and
    exported-declaration identities, the exact TargetProfile, the compiler
    build identity, and the build configuration selectors. A candidate is
    accepted only when the store-provided key equals the request's expected
    key, the current SemanticPlan verifies against the exact ordered dependency
    plan vector, and the candidate decodes and independently verifies against
    those same authorities. Its fingerprint must equal the plan the requesting
    operation already produced, so a hit is evidence of unchanged content and
    never a substitute for a stage. A missing verifier context, a mismatched
    key, plan, or dependency vector, or mutated bytes fail closed. The
    native build consults the store only after every module owns a verified
    plan. It derives an exact SCC task graph in the compiler session, lets
    workers prepare disjoint XSM artifact rows from immutable plan authority,
    joins every level, and preflights every candidate byte sequence with the
    independent XSM verifier before publication begins. Only then are misses
    published in canonical task/member order and the candidate dependency graph
    installed. Worker completion order cannot affect artifact bytes,
    diagnostics, recomputed-module counts, or publication order. Absence of
    program provenance is a distinct zero-authority key state and cannot alias
    a plan that owns program provenance; it is never reconstructed from a path,
    name, or dependency spelling. A hit skips
    re-encoding and re-publication alone, while a miss or rejected candidate
    prepares a freshly encoded artifact. Because the key names the ordered
    dependency module/fingerprint digest independently, a source import or
    module-set resolution change necessarily selects a different XSM address;
    the stale cache identity cannot be accepted as the new request. If a later
    cache I/O fails after an earlier canonical publish, the session does not
    install the graph or advance the workspace generation. The earlier object
    is an immutable exact-key orphan with no failed-generation address; any
    later operation must derive the same key and independently verify the raw
    bytes again before it can count as a hit.
12. For the bounded two-source-module scalar product graph, the native source
    driver publishes one independently verified schema-v5
    `SCALAR_MODULE_GRAPH_DIRECT_CALL` PSC/GCI before Xi lowering and
    carries that immutable authority into module-summary construction. Each
    graph spec must match exactly one PSC module row through canonical source
    semantic module authority; duplicate, missing, stale, or foreign rows abort
    publication. Its typed selective dependency freezes the exact import
    locator, exported declaration/function, canonical dependency export
    fingerprint, and resolver binding; the cross-module call carries the same
    binding, and the verifier independently rebuilds every join. The complete
    product PSC fingerprint and GCI are copied into
    every module's XSM key, so a dependency source change rotates both module
    addresses even when the entry source is unchanged. An existing per-plan
    provenance row may only confirm the identical PSC/GCI. The claimed product
    predicate cannot fall back to the zero-authority key state, a path/name
    reconstruction, an old key reader, or a second cache lookup path.

Changing the root-lock coverage, rejected-snapshot identity, quota reservation
order, atomic publication sequence, directory/link boundary, lock cleanup,
XSM summary derivation, task preflight/publication ordering, or mandatory
verification is a contract change.

## Digest anchors

anchor-sha256: src/incremental/xr_cache_artifact_verify.h 18a284780d2154a14b53ba5591140b92fa8555f6373b503ffa019f59bafc6026
anchor-sha256: src/incremental/xr_cache_artifact_verify.c a0b359e7aaa5042eaf6d1c1eb6d52db253fca91dad6ae7d18995585f50be9df3
anchor-sha256: src/incremental/xr_cache_store.h f34e4f86ba65f44cbc29356488f32cbc52088c8dda6848ff756a571c78c9b1d9
anchor-sha256: src/incremental/xr_cache_store.c bb726097541fb71d58d463f106bc7f103c21295ffee344425221b67a094d305b
anchor-sha256: src/incremental/xr_target_plan_tasks.h b8898e83f64a5f3526199de7e2cdc1de081ccaf677007c2adf65b641b92aff5a
anchor-sha256: src/incremental/xr_target_plan_tasks.c 97cb9d24a31e852506ace288f2b3c72fd7828a7cc1a1adb161f7a6a3c85377cc
anchor-sha256: src/incremental/xr_module_summary_build.h 1d387ea9e943fa0fcebeba7222105b8d0677bdecf05cda3677dc0d400868279b
anchor-sha256: src/incremental/xr_module_summary_build.c 0a58ea617bb715b7448fc57c51780e1ddb99dfe8e9bfbb38001abfb28ed29cc2
anchor-sha256: src/aot/xaot_module_summary.h ab160517cfb59565b24f75f1273afb08e0c5d6c2370f282a1c09c7f45846adcc
anchor-sha256: src/aot/xaot_module_summary.c e8ad587dbe89d9b8e814851c04f9fa5756a6f6fa30776aa436fbf7067237673d
anchor-sha256: src/aot/xaot_driver.h 8f2f4d10c58b2d2e37f0a1806b5c0b29814c343c69b1a2ff4f51633ec7a9392d
anchor-sha256: src/aot/xaot_driver.c 862e1ac8a866931d8a5c7a86262657c986480af28da0bc6842322dea2f5b4bf6
anchor-sha256: src/os/os_fs.h 9b1c4d8779dbe274049c8eafbc887501cb5131c82e15170d56663a0b74a7b253
anchor-sha256: src/os/unix/fs_unix.c fe178220141229044606cba6e2dc0df6a80767e07b43c93ede189b74434569ef
anchor-sha256: src/os/win/fs_win.c 2ca47d9c0ce3b0b2b999e5dcbc2f855e1b6e80113e32625aa82940cb4450c104
anchor-sha256: tests/unit/CMakeLists.txt 405e5d564669aeb8e1ad1ac31e14613a8530fd89b63247940a6d38748bdd1ba8
anchor-sha256: tests/unit/incremental/test_cache_artifact_verify.c c9819052135b26f8717a9746e3b496a6b4b799be54e8c5f9fd6fb83235ee549c
anchor-sha256: tests/unit/incremental/test_cache_store.c 927f5058b962d5cda2471a14aed9d03730daba396cd6934e7b9add8fd8128618
anchor-sha256: tests/unit/incremental/test_target_plan_tasks.c 558d2917133a9dee2206b997d0a12139b63d561865431d960889c8d0d5b87d3c
anchor-sha256: tests/unit/incremental/test_module_summary_build.c 117a7c617160868de286912287b70b97dc79d07f685b740deb08d1e79e3f704e
anchor-sha256: tests/aot/run_module_summary_determinism.py 07cb7ca7060d15d8d8a85fdce7abd555d3207eb977f820029f4691a0b3c6b239
anchor-sha256: tests/unit/aot/test_xaot_driver.c 717147203998c04a1ab1653756994cbceb5ea37a138e02898cb5fd99505dee65
anchor-sha256: tests/unit/os/test_fs_atomic.c 3166bf0113590778cf46f874a7205476819e043699bb99d9881549579237ce12
