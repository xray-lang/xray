/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * program_plan_cache_fixture.h - Shared program TargetPlan cache test authority
 *
 * KEY CONCEPT:
 *   One fixture states the semantic authority, the store configuration, and
 *   the canonical-bytes comparison that every program plan cache test depends
 *   on.  A second private copy of any of them would let one suite drift from
 *   the behaviour another suite pins.
 */

#ifndef PROGRAM_PLAN_CACHE_FIXTURE_H
#define PROGRAM_PLAN_CACHE_FIXTURE_H

#include "incremental/xr_program_target_plan_build.h"
#include "incremental/xr_cache_store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrSemanticPlan XrSemanticPlan;
typedef struct XrTargetPlan XrTargetPlan;
typedef struct XrTargetProfile XrTargetProfile;

typedef struct XrTestProgramPlanBuildInput {
    const XrSemanticPlan *semantic;
    const XrSemanticPlan *const *dependencies;
    uint32_t dependency_count;
    XrTargetProfile *profile;
    XrCacheStore *store;
    bool rebuild;
    XrProgramTargetPlanCancellationToken *cancellation;
} XrTestProgramPlanBuildInput;

typedef struct XrTestProgramPlanStoreInventory {
    size_t objects;
    size_t temps;
    uint64_t object_bytes;
} XrTestProgramPlanStoreInventory;

/* Build the deterministic single-module semantic authority named by `ordinal`.
 * Equal ordinals derive one identical cache key, which is what lets a caller
 * prove that cold, warm, and parallel builds agree on one plan. */
XrSemanticPlan *xr_test_program_plan_semantic(uint32_t ordinal);

/* Open the shared store.  The budget is stated by the caller so a test that
 * qualifies an over-budget refusal never depends on an implicit default. */
XrCacheStore *xr_test_program_plan_store_open(const char *root, uint64_t quota_bytes,
                                              size_t max_entry_bytes);

/* Remove every object, temp residue, and lock file the store owns. */
void xr_test_program_plan_store_remove(const char *root);

/* Count published objects and unfinished temp residue under `root`. */
void xr_test_program_plan_store_inventory(const char *root, XrTestProgramPlanStoreInventory *out);

/* Run one program TargetPlan build against the shared optimization budget. */
bool xr_test_program_plan_build(const XrTestProgramPlanBuildInput *input,
                                XrProgramTargetPlanBuildResult *result, char *error,
                                size_t error_size);

/* Encode `plan` into the canonical XTP bytes two results are compared by.
 * A successful call transfers ownership; release it with the free below. */
bool xr_test_program_plan_encode(const XrTargetPlan *plan, uint8_t **bytes, size_t *size);
void xr_test_program_plan_encoded_free(uint8_t *bytes);

#endif  // PROGRAM_PLAN_CACHE_FIXTURE_H
