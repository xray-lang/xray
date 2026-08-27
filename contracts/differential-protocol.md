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
    match it, and freezes the exact hosted native
    provider and artifact reported by `toolchain doctor --json`. Each refusal
    retains its complete raw native build log, the first source-emitted refusal,
    every reached refusal, and the emitting SemanticPlan verifier, TargetPlan
    builder, or AOT representation owner. Missing identity, zero cases,
    differential failure, a refusal without source-emitted owner/fact evidence,
    or a raw-log/hash/aggregation mismatch fails the manifest; none can become
    skip. Direct-local call refusals include the deciding opcode, parameter
    ordinal, admitted-storage mask, modes, ownership, transfer, access, role,
    contract and type/ordinal equality facts; result refusals include arity,
    result-type equality and the independently admitted-storage mask, so unlike
    call shapes do not collapse under one generic diagnostic. Source-declared
    backend exclusions remain skips, and the two governed
    VM-plus-native-rejection oracles are reported as expected rejections rather
    than as comparable executions. Verification independently rediscovers the
    inputs, rechecks the current compiler/provider, reparses raw logs, and
    reconstructs first-refusal and root-cause aggregates. The canonical refusal
    coverage ratchet remains a qualification gate: the manifest records every
    newly refusing and every newly building case separately, and either drift
    keeps its status failed until the source or governed shrinking baseline is
    corrected; generating the census never rewrites that baseline.

## Digest anchors

anchor-sha256: tests/diff/run_backend_diff.py 9b30566a476c0816a1939f41a281cbac3709b2062482ddcc0620b72b7424484d
anchor-sha256: tests/diff/survey_refusals.py 586537bf095ee10aeb3f7f0a4256f7ae896b83b5e763ec19440c137cfc73f4a5
anchor-sha256: scripts/check_live_refusal_manifest.py 004e1f66aebc1d1c58ccece66ceb8d1d7bbcdae3e7590f3582b5cea7007e0637
anchor-sha256: src/plan/semantic/xr_semantic_verify.c f255b5868f54b94bcb2c95a601d94eb30fcfb10730a7962d2d527acb45fdaf1f
anchor-sha256: src/plan/target/xr_target_builder.c 76b3fac066b1b5b9313f0537be787808fe7c2d5c3e7c4d82deb8cdec828844c5
anchor-sha256: src/aot/refine/xr_aot_representation_refinement.c 37138428d686308954349f148e7bb6a484aef3be05480d6e5bb61cc261a73ea6
anchor-sha256: tests/aot/TOMBSTONES.tsv 1ad7d280093c5a3aedecdf490fe88dc9c48f79215de9ea1d1c8216373cd56eb7
