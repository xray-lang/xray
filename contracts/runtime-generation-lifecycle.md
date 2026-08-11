# Runtime module generation lifecycle contract

This contract freezes the runtime-owned authority for loaded TargetPlan
generations. It defines lifecycle and resource ownership only; it does not
claim that a typed executor, export resolver, call engine, or root scanner is
installed.

1. A generation authority requires explicit, nonzero hard limits for loaded
   generations, total pins, per-generation pins, and every pin kind. The same
   runtime mutex serializes generation allocation, pin acquisition/release,
   retirement, unload, and authority destruction, so concurrent attempts
   cannot oversubscribe a checked budget.
2. Loading accepts only an independently verified immutable TargetPlan. The
   runtime assigns a monotonic nonzero generation number and derives a stable
   SHA-256 generation identity from the plan schema, completed family mask,
   exact capability mask, semantic/profile/plan fingerprints, and the native
   runtime/provider/object-header fingerprints. No pointer, path, host name,
   legacy bytecode, or caller-authored machine fact participates in authority.
3. The normal state order is exactly `LOADING -> VERIFIED -> READY -> ACTIVE ->
   DRAINING -> RETIRED -> UNLOADED`. A healthy ACTIVE generation alone may
   acquire pins. ACTIVE and DRAINING may release matched pins. Retirement and
   unload require every generic, in-flight call, callback, destructor, and
   static-root pin to be zero.
4. Poisoning requires a nonzero stable diagnostic fingerprint and cannot be
   overwritten by a conflicting fingerprint. Rollback is an explicit failure
   branch: a pre-activation generation becomes RETIRED with zero pins, ACTIVE
   becomes DRAINING, and DRAINING remains drainable. A poisoned or rolling-back
   generation cannot become READY or ACTIVE.
5. READY and ACTIVE fail with `XR_EXEC_5004` while typed executor, call, and
   root families have no installed production authority. There is no
   compatibility VM, XRC translation, generic tagged-frame execution, or
   test-only activation bypass.
6. The independent verifier re-derives immutable plan/native identity, stable
   generation fingerprint, state/poison invariants, counter sums, per-kind and
   global budgets, and current executor availability. Its separate transition
   model rejects identity mutation, skipped/repeated states, mismatched pins,
   illegal rollback, and revision mutation.
7. The standalone public header and all public lifecycle symbols are shipped
   by the Core component and link from `xray_vm_runtime` without compiler
   builders, encoders, analyzer objects, or Xi implementation symbols.

anchor-sha256: include/xray_runtime_generation.h 12a5102eda43645773d7a3fc00b2a9f1f80d21068582d326c75558f49686d260
anchor-sha256: src/runtime/xr_module_generation_internal.h 427d2d23bfa8991dd8d20463169fb9bb23d7486a38d5274cb5d84b089a14a96a
anchor-sha256: src/runtime/xr_module_generation.c 6681c374c912ee70bcabdbf7836598caa9e54108c0ea6ffebca16e607241cea0
anchor-sha256: src/runtime/xr_module_generation_verify.c 53ed112f57405bce3707323038a434df7345d6d4aa12d8a369253fe025b48c85
anchor-sha256: contracts/target-machine/diagnostic-codes.toml ad9c96db3c85a41af4649304375ed92433f84ee8993027c1511b2247ee36e73f
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 500cd83525f2c8068438626866cd435d7935d5d2423670ee8bbbbab26def2129
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 09d8a06b19706a19f82ced3b03dc8813215b636265e663b59b18d852ce3a1d79
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 0fee8f623d69002b2daaa6f2fdf99ce315bf85b0b0955caf92aeb8f9982684e0
anchor-sha256: tests/install/run_install_public_surface_tests.py 9ef54ccfd07d2bf8c40d2ca5065248e7667899a291a3e9cbaba74aafd067ef47
