# Differential and oracle protocol contract

Status: re-frozen by tasks 256 and 257. Manifest, argument, and baseline
sidecars are decoded explicitly as UTF-8 on every host; subprocess capture and
the program-output comparison remain byte-native and are not normalized.

1. Backend differential equivalence compares observable exit status and stdout
   byte-for-byte. Normalized stderr is compared only when the lane explicitly
   enables that channel; build logs are not program output. The harness must not
   decode any form's program output before this comparison.
2. Three execution forms are subject to this contract, and a lane names the ones
   it compares: `xray run` (VM), `xray build --native` (AOT), and the default
   `xray build`, whose binary carries the program as bytecode serialized into
   the constant-pool container. The default build is a compared form, not a
   packaging detail: it reaches the program through a serialize/deserialize
   round trip no other form performs.
3. A checked-in `.expected` sidecar is an exact stdout oracle and requires a
   zero exit status. A `.stdin` sidecar supplies identical bytes to each form.
   A directory `xray.toml` is part of the case identity and supplies the same
   NativePackagePlan to every lane.
4. KAT/oracle data records its upstream version or generator, uses deterministic
   inputs, and is evaluated independently of the implementation under test.
5. Fuzz failures preserve seed/corpus evidence sufficient for exact replay.
6. A tombstone classifies a known failing verification asset; it does not alter
   semantic equivalence. Tombstones may only shrink, and a newly passing case is
   removed rather than reclassified.
7. Changing compared channels, compared forms, normalization, expected-output
   behavior, oracle provenance, or tombstone meaning is a contract change and
   requires all dependent suites/ports to be revalidated.
8. A differential measurement requires an executable reference binary, the
   governed case directory, and at least one runnable case in the selected
   shard. Missing identity or zero-case selection is a failed measurement, not
   a skip or a successful empty comparison.
9. A form that refuses to build a case produces no program output, so the case
   yields no equivalence verdict and is not a divergence. Refusals are
   classified apart from cases where every compared form ran and their output
   disagreed, and each classification carries its own baseline. Both ratchet
   the same way: the list may only shrink, a case leaving one list deletes its
   line rather than moving to the other, and a case that stops building is a
   regression even though it states nothing about equivalence. Conflating the
   two is what made this net unreadable while the native backend was
   fail-closed ahead of its per-construct authorities: on the native lane 564
   refusals hid a single real divergence, and on the embedded lane 68 hid nine.
10. Ratchet case identity is the repository-relative path written with `/`
    separators on every host. Native path separators are non-semantic checkout
    details. Result names, divergence baselines, and refusal baselines must all
    consume this one canonical spelling so the same governed case cannot leave
    and re-enter a baseline solely because the runner changed operating system.
11. A live refusal/root-cause manifest is qualification evidence from one full,
    unsharded invocation of the canonical VM/AOT differential runner; it is not
    a baseline, waiver, active-generation manifest, or alternate case verdict.
    The generator auto-discovers every governed case and oracle through that
    runner, requires a clean source tree and its exact Ninja Release,
    VM-fastpaths-off compiler whose `--version --json` commit and dirty state
    match it, and freezes the exact hosted native provider and artifact reported
    by `toolchain doctor --json`. The live refusal manifest uses schema 3
    exclusively; the generator and checker reject every other schema without an
    alias, compatibility reader, or fallback. The diagnostic registry is the
    exact set of `id` values in TOML `[[code]]` entries, including numbered and
    named identifiers. A missing, malformed, or duplicate identifier fails
    closed.

    Every source-emitted refusal row binds exactly one registered diagnostic
    code found on that same raw-log line. Zero or multiple registered codes on a
    row remain explicit evidence debt; a diagnostic elsewhere in the log cannot
    satisfy that row. Each refusal retains its complete raw native build log,
    every reached refusal, and the emitting SemanticPlan verifier, TargetPlan
    builder, or AOT representation owner. `first_refusal` is exactly row zero of
    the ordered `refusals` array. Row-local bindings are the refusal-evidence
    completeness owner; no result-level diagnostic or evidence-gap surrogate can
    satisfy completeness. Non-refusal runner, provider, or differential failures
    remain a separate outcome class.

    Missing identity, zero cases, differential failure, a refusal without
    source-emitted owner/fact evidence, or a raw-log/hash/aggregation mismatch
    fails the manifest; none can become skip. Direct-local call refusals include
    the deciding opcode, parameter ordinal, admitted-storage mask, modes,
    ownership, transfer, access, role, contract and type/ordinal equality facts;
    result refusals include arity, result-type equality and the independently
    admitted-storage mask, so unlike call shapes do not collapse under one
    generic diagnostic. Source-declared backend exclusions remain skips, and the
    two governed VM-plus-native-rejection oracles are reported as expected
    rejections rather than as comparable executions. Verification independently
    rediscovers the inputs, rechecks the current compiler/provider, reparses raw
    logs, and reconstructs first-refusal, row-local diagnostic, evidence-gap,
    root-cause, and summary aggregates.

    Qualification can pass only when every refused case has at least one refusal
    row, every row has exactly one valid same-row binding, and the identity,
    ordering, raw-log hash, aggregation, and coverage-ratchet checks all pass.
    The canonical refusal coverage ratchet remains a qualification gate: the
    manifest records every newly refusing and every newly building case
    separately, and either drift keeps its status failed until the source or
    governed shrinking baseline is corrected. A current census with incomplete
    row evidence remains failed and reports the exact debt; census generation
    does not change its denominator or rewrite the governed refusal baseline.

## Digest anchors

anchor-sha256: tests/diff/run_backend_diff.py 25e7a5cb894f419008eeffc1369f2c9567deb090492763b0a25eaaff959013e9
anchor-sha256: tests/diff/survey_refusals.py 50a54dcdd631d73c03d309b7fcbfb77f9ae22b64cad86847d96ea36916c373e3
anchor-sha256: scripts/check_live_refusal_manifest.py 81f489f08175034643871675d246a09bf4e70cbae5a6a9e9116ec8e1edebebea
anchor-sha256: src/plan/semantic/xr_semantic_verify.c 6ab9f65a8ee2a89075f865156a63bd67dcda4c18d1c5446baf69984b8a29c8bd
anchor-sha256: src/plan/target/xr_target_builder.c f41c1ffd03836ecdf1ae442d1260f21425360c37f2ef09b70c3b20f6fbd76020
anchor-sha256: src/aot/refine/xr_aot_representation_refinement.c 77424eac74ebcbe34bb4d98a5eb4f55c7c9266cc55cc00fda5fad5482cf47db5
anchor-sha256: src/aot/refine/xr_aot_scalar_ref_v1.h ff60dac943a74d84c08f125195c857431d97fffaf4e61d97d2e501a714afc38b
anchor-sha256: src/aot/refine/xr_aot_scalar_ref_v1.c ef79278f61d49194f0f9cd3f170602f28a52bb282e8ff8e4fb3fde24fad47f16
anchor-sha256: tests/aot/TOMBSTONES.tsv 1ad7d280093c5a3aedecdf490fe88dc9c48f79215de9ea1d1c8216373cd56eb7
