/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_forest_seal_cases.h - Fail-closed execution closure publication
 */
#ifndef XIR_MODULE_FOREST_SEAL_CASES_H
#define XIR_MODULE_FOREST_SEAL_CASES_H
static XrXirProgram *module_forest_seal(const XrXirProgramSpec *spec) {
    const XrXirProgramBudget limits = {2097152, 16000000};
    XrXirProgram *program = NULL;
    CHECK(!runtime_live && !runtime_bytes);
    runtime_attempts = 0;
    CHECK(xr_xir_program_seal(spec, limits, &program) == XR_XIR_OK);
    size_t attempts = runtime_attempts;
    xr_xir_program_drop(program); program = NULL;
    CHECK(!runtime_live && !runtime_bytes);
    for (size_t fail = 0; fail < attempts; ++fail) {
        runtime_attempts = 0; runtime_fail_at = fail;
        XrXirStatus status = xr_xir_program_seal(spec, limits, &program);
        runtime_fail_at = SIZE_MAX;
        CHECK(status != XR_XIR_OK && !program);
        CHECK(!runtime_live && !runtime_bytes);
    }
    for (unsigned kind = 0; kind < 2; ++kind) {
        uint64_t low = 0, high = kind ? limits.work : limits.metadata_bytes;
        while (low < high) {
            uint64_t middle = low + (high - low) / 2;
            XrXirProgramBudget budget = limits;
            if (kind) budget.work = middle; else budget.metadata_bytes = middle;
            XrXirStatus status = xr_xir_program_seal(spec, budget, &program);
            if (status == XR_XIR_OK) {
                CHECK(program); xr_xir_program_drop(program); program = NULL; high = middle;
            } else {
                CHECK(status == XR_XIR_BUDGET && !program); low = middle + 1;
            }
            CHECK(!runtime_live && !runtime_bytes);
        }
        CHECK(low > 0);
        for (unsigned below = 0; below < 2; ++below) {
            XrXirProgramBudget budget = limits;
            if (kind) budget.work = low - below; else budget.metadata_bytes = low - below;
            XrXirStatus status = xr_xir_program_seal(spec, budget, &program);
            CHECK(status == (below ? XR_XIR_BUDGET : XR_XIR_OK));
            if (below) CHECK(!program);
            xr_xir_program_drop(program); program = NULL;
            CHECK(!runtime_live && !runtime_bytes);
        }
    }
    printf("Module forest seal: %zu allocation failures, exact byte/work boundaries, zero residuals\n", attempts);
    CHECK(xr_xir_program_seal(spec, limits, &program) == XR_XIR_OK);
    return program;
}
#endif // XIR_MODULE_FOREST_SEAL_CASES_H
