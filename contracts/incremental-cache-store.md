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
9. Program TargetPlan build/cache has one request and one result. The request
   names one verified root SemanticPlan, its exact ordered dependency-plan
   vector, one exact TargetProfile, and one optimization budget. It has no
   module task array, worker limit, per-module result row, or canonical task
   index. Authority is verified before cache lookup or planning. A cold miss
   and an explicit rebuild construct the same program plan through the direct
   TargetPlan builder; a warm hit independently materializes the same program
   plan from XTP. Only a complete verified plan can enter the result, and the
   AOT driver installs that single owned plan into the bundle.
10. Cancellation is a monotonic build-owned token. The direct program build
    checks it before authority validation, after cache materialization, after
    planning, and after publication. Once requested, the operation releases
    its owned plan and returns no partial result. A verified immutable cache
    object published before the request remains available to later operations;
    cancellation never deletes another operation's content-addressed entry.
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
    `SCALAR_MODULE_GRAPH_DIRECT_CALL` PSC/GCI before Xi lowering and carries
    that immutable authority into two Xi partitions and two SemanticPlan 43
    artifacts. Each
    graph spec must match exactly one PSC module row through canonical source
    semantic module authority; duplicate, missing, stale, or foreign rows abort
    publication. Its typed selective dependency freezes the exact import
    locator, exported declaration/function, canonical dependency export
    fingerprint, and resolver binding; the cross-module call carries the same
    binding, and the Xi/SemanticPlan module-set verifiers independently rebuild
    every live module, function, import/export, resolver, call, attachment, and
    dependency join. Producer and entry XSM bytes are deterministic; entry
    decode requires the exact ordered producer plan. The driver then constructs
    one independently verified schema-49 program TargetPlan from the full
    canonical SemanticPlan module set. Its single graph row, two pointer-free
    module partitions, aggregate semantic fingerprint, global Target rows, and
    exact `PROGRAM_DIRECT`/`CALL_DIRECT_I64` edge must all verify before
    module-summary cache authority may publish. The complete product PSC
    fingerprint and GCI are copied into
    every module's XSM key, so a dependency source change rotates both module
    addresses even when the entry source is unchanged. An existing per-plan
    provenance row may only confirm the identical PSC/GCI. The claimed product
    predicate cannot fall back to the zero-authority key state, a path/name
    reconstruction, an old key reader, or a second cache lookup path. Cache
    publication grants no VM or AOT execution authority: after the module
    summaries are published, the product runner must fail at
    `XR_TARGET_1001: verified program TargetPlan execution is outside same-plan
    VM/AOT coverage` and produce no binary. Cold, warm, dependency-edit, and
    dependency-revert runs must retain the same cache publication and
    invalidation behavior at this explicit execution fence.

Changing the root-lock coverage, rejected-snapshot identity, quota reservation
order, atomic publication sequence, directory/link boundary, lock cleanup,
XSM summary derivation, task preflight/publication ordering, or mandatory
verification is a contract change.

## Digest anchors

anchor-sha256: src/incremental/xr_cache_artifact_verify.h 18a284780d2154a14b53ba5591140b92fa8555f6373b503ffa019f59bafc6026
anchor-sha256: src/incremental/xr_cache_artifact_verify.c a0b359e7aaa5042eaf6d1c1eb6d52db253fca91dad6ae7d18995585f50be9df3
anchor-sha256: src/incremental/xr_cache_store.h f34e4f86ba65f44cbc29356488f32cbc52088c8dda6848ff756a571c78c9b1d9
anchor-sha256: src/incremental/xr_cache_store.c bb726097541fb71d58d463f106bc7f103c21295ffee344425221b67a094d305b
anchor-sha256: src/incremental/xr_program_target_plan_build.h fce79b35699dec7f231248b891a2ebcb1ebf7ef185ace53e6cd74a314f7bc610
anchor-sha256: src/incremental/xr_program_target_plan_build.c 28c400a25e18988fe9b538675ff862f23ef7eec3c65307fe8212450b4dc3386b
anchor-sha256: src/incremental/xr_module_summary_build.h 1d387ea9e943fa0fcebeba7222105b8d0677bdecf05cda3677dc0d400868279b
anchor-sha256: src/incremental/xr_module_summary_build.c 0a58ea617bb715b7448fc57c51780e1ddb99dfe8e9bfbb38001abfb28ed29cc2
anchor-sha256: src/aot/xaot_module_summary.h ab160517cfb59565b24f75f1273afb08e0c5d6c2370f282a1c09c7f45846adcc
anchor-sha256: src/aot/xaot_module_summary.c c618aecfcbadf2cd1b0c6d17d4e05760dcd5111f63d823d7fe672a104f32dc31
anchor-sha256: src/aot/xaot_driver.h bbf2dba4ad268d09cf45c32080a79202e85355600a793ea3571462822719e88d
anchor-sha256: src/aot/xaot_driver.c 4c2c8bf0323d6d59c00647dc24f996f8ffb3d6d43e9b29b764336320f67bb7d1
anchor-sha256: src/os/os_fs.h 9b1c4d8779dbe274049c8eafbc887501cb5131c82e15170d56663a0b74a7b253
anchor-sha256: src/os/unix/fs_unix.c fe178220141229044606cba6e2dc0df6a80767e07b43c93ede189b74434569ef
anchor-sha256: src/os/win/fs_win.c 2ca47d9c0ce3b0b2b999e5dcbc2f855e1b6e80113e32625aa82940cb4450c104
anchor-sha256: tests/unit/CMakeLists.txt 82625a435d47134b938a40675265424b5f06d4d4eb59a4d1d1306eafaacc53c8
anchor-sha256: tests/unit/incremental/test_cache_artifact_verify.c c9819052135b26f8717a9746e3b496a6b4b799be54e8c5f9fd6fb83235ee549c
anchor-sha256: tests/unit/incremental/test_cache_store.c 927f5058b962d5cda2471a14aed9d03730daba396cd6934e7b9add8fd8128618
anchor-sha256: tests/unit/incremental/test_program_target_plan_build.c 55297a8a70bcb0c463dd03512699ecd7bc7723fd505ac75b102c585ed4c4857f
anchor-sha256: tests/unit/incremental/test_module_summary_build.c 117a7c617160868de286912287b70b97dc79d07f685b740deb08d1e79e3f704e
anchor-sha256: tests/aot/run_module_summary_determinism.py ab111483920e1059375226da9d9cf84fb96e2474c0608418083aff403cf85d87
anchor-sha256: tests/unit/aot/test_xaot_driver.c 067bc64b28de1e058dc91b49bd78d80c290bfdab73cd97bfdb3e2fc98920339d
anchor-sha256: tests/unit/os/test_fs_atomic.c 3166bf0113590778cf46f874a7205476819e043699bb99d9881549579237ce12
