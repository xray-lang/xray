/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_fi_shards.inc.c - Complete receiver allocation ordinal dispatch
 *
 * KEY CONCEPT:
 *   Every shard measures the same real normal sequence and then stops at each exact owned refusal.
 */
static void typed_fi_shard_summary(bool pass, unsigned kind, unsigned index, unsigned jobs,
    size_t total, size_t covered) {
    printf("typed-library-fi shard-summary result=%s kind=%s mode=%u shard=%u shards=%u "
        "total=%zu covered=%zu ordinal-start=%u ordinal-step=%u physical=%u\n",
        pass ? "PASS" : "FAIL", kind == TFI_COMPILER ? "compiler" : "runtime",
        typed_fi.run.mode, index, jobs, total, covered, index, jobs, (unsigned)typed_fi.physical_zero);
    fflush(stdout); /* Partial completed ordinals survive timeout; not a full qualification. */
}
static int typed_fi_shard(char **argv, TypedFiRequest request, unsigned mode) {
    size_t index = 0, jobs = 0, compiler = 0, runtime = 0;
    if (!typed_fi_number(argv[6], &index) || !typed_fi_number(argv[7], &jobs) ||
        !typed_fi_number(argv[8], &compiler) || !typed_fi_number(argv[9], &runtime) ||
        !compiler || !runtime || jobs != 8 || index >= jobs) return 2;
    unsigned kind = !strcmp(argv[5], "--compiler-shard") ? TFI_COMPILER : TFI_RUNTIME;
    size_t total = kind == TFI_COMPILER ? compiler : runtime, covered = 0;
    bool pass = typed_fi_run(argv, request, mode);
    pass = pass && source_program_compile_attempts == compiler && runtime_attempts == runtime;
    for (size_t site = index; pass && site < total; ) {
        request.kind = kind; request.site = site; request.total = total;
        pass = typed_fi_run(argv, request, mode);
        printf("typed-library-fi shard-case result=%s kind=%s mode=%u shard=%zu ordinal=%zu\n",
            pass ? "PASS" : "FAIL", kind == TFI_COMPILER ? "compiler" : "runtime", mode, index, site);
        fflush(stdout);
        if (pass) ++covered;
        if (total - site <= jobs) break; /* No size_t wrap at the last ordinal. */
        site += jobs;
    }
    typed_fi_shard_summary(pass, kind, (unsigned)index, (unsigned)jobs, total, covered);
    return pass ? 0 : 1;
}
