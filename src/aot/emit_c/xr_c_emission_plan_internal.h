/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_plan_internal.h - Private C emission plan storage
 *
 * KEY CONCEPT:
 *   The public plan remains opaque. Verifier mutation tests include this
 *   private definition so their field writes cannot drift from production
 *   storage and corrupt an unrelated owned pointer.
 */

#ifndef XR_C_EMISSION_PLAN_INTERNAL_H
#define XR_C_EMISSION_PLAN_INTERNAL_H

#include "xr_c_emission_plan.h"

struct XrCEmissionPlan {
    XrCValueEmissionView *values;
    uint32_t value_count;
    XrCCallArgumentEmissionView *call_arguments;
    uint32_t call_argument_count;
    XrCRecipeArgumentView *recipe_arguments;
    uint32_t recipe_argument_count;
    XrCCleanupEmissionView *cleanups;
    uint32_t cleanup_count;
    XrCFunctionAbiEmissionView *function_abis;
    uint32_t function_abi_count;
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrFingerprint semantic_fingerprint;
    XrFingerprint profile_fingerprint;
    XrFingerprint fingerprint;
    bool verified;
};

#endif  // XR_C_EMISSION_PLAN_INTERNAL_H
