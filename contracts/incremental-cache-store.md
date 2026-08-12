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
8. An XTP cache request derives its key from the verified SemanticPlan, exact
   TargetProfile, provider and runtime identities owned by that profile, the
   current planner schema and required-family mask, and the requested
   optimization budget. A matching object is still only a candidate: the
   verifier takes an owned XTP snapshot, materializes it against the exact
   authorities, and independently verifies the resulting TargetPlan before
   accepting the hit. The production AOT driver consumes that exact owned
   plan; it does not decode a second time or rebuild semantic rows on a hit.
   Wrong keys, authority mismatches, old schemas, corrupt bytes, and valid
   artifacts for another semantic or target identity fail closed to
   recomputation.

Changing the root-lock coverage, rejected-snapshot identity, quota reservation
order, atomic publication sequence, directory/link boundary, lock cleanup, or
mandatory verification is a contract change.

## Digest anchors

anchor-sha256: src/incremental/xr_cache_artifact_verify.h c70b37fb819f7bf64af9b4b968cb770bae8693011e95353a0c5710e3f1ab6a2e
anchor-sha256: src/incremental/xr_cache_artifact_verify.c 90cc79eeffe2766ebd494d0056b4b0ede9681b846537714327a7428d01adeced
anchor-sha256: src/incremental/xr_cache_store.h f34e4f86ba65f44cbc29356488f32cbc52088c8dda6848ff756a571c78c9b1d9
anchor-sha256: src/incremental/xr_cache_store.c bb726097541fb71d58d463f106bc7f103c21295ffee344425221b67a094d305b
anchor-sha256: src/os/os_fs.h b1a95259a4952a1e33e1d2c109fd0955f5b00bff4db81e6a21abede9ec07fe84
anchor-sha256: src/os/unix/fs_unix.c 5f86aaa44d1e794ca2cdeea814c1d115bd598e56b3e7dfe47d0c6726da187e54
anchor-sha256: src/os/win/fs_win.c 4b9195ab156d94761c6c72c10a80fdb62a01838d1ec829ab42bbb5bc9bd71e2f
anchor-sha256: tests/unit/CMakeLists.txt 3b70eacbac8d8c3001e6020c12c30103d55d630830949e0ab40af5ef18a8524a
anchor-sha256: tests/unit/incremental/test_cache_artifact_verify.c 8a7a0f35523e2f7b846a84e22f42521d43c2e70d0eb4342d4a3d13c58dfc1fb4
anchor-sha256: tests/unit/incremental/test_cache_store.c 927f5058b962d5cda2471a14aed9d03730daba396cd6934e7b9add8fd8128618
anchor-sha256: tests/unit/os/test_fs_atomic.c f8f6ee065dcb3c4b75ae24edb4e96d95253c540f5a40252cf79a10aa140ddb5f
