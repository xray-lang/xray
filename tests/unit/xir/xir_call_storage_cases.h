/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_call_storage_cases.h - Call vectors retain one source arena owner
 *
 * KEY CONCEPT:
 *   Coallocation changes ownership storage, never inference work or authority.
 */
static void source_call_storage_cases(void) {
    size_t baseline = live;
    for (unsigned mode = 0; mode < 7; ++mode) {
        SourceContext ctx = {0}; ctx.budget = xr_xir_default_budget();
        ctx.budget.metadata_bytes = sizeof(SourceMemory) + sizeof(XrXirType) + sizeof(SourceValue);
        uint64_t work = ctx.budget.work, scratch = ctx.budget.scratch_bytes;
        if (mode == 1) --ctx.budget.metadata_bytes;
        if (mode == 2) ++ctx.budget.metadata_bytes;
        size_t start = attempts; fail_at = mode == 3 ? start : SIZE_MAX;
        SourceCallStorage output = {(XrXirType *)(uintptr_t)1, (SourceValue *)(uintptr_t)1};
        size_t types = mode == 4 ? SIZE_MAX : mode == 6 ? 0 : 1;
        uint64_t values = mode == 5 ? UINT64_MAX : mode == 6 ? 0 : 1;
        bool success = source_call_storage(&ctx, types, values, &output);
        CHECK(success == (mode == 0 || mode == 2 || mode == 6));
        CHECK(ctx.budget.work == work && ctx.budget.scratch_bytes == scratch);
        if (success && mode != 6) {
            CHECK(output.types && output.values && ctx.memory && !ctx.memory->next);
            CHECK((unsigned char *)output.values == (unsigned char *)output.types + sizeof(XrXirType));
            CHECK(attempts == start + 1);
        } else {
            CHECK(!output.types && !output.values && !ctx.memory);
            if (!success) CHECK(ctx.diagnostic.status == (mode == 3 ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET));
        }
        while (ctx.memory) {SourceMemory *next = ctx.memory->next; xr_free(ctx.memory); ctx.memory = next;}
        CHECK(live == baseline);
    }
    fail_at = SIZE_MAX;
}
