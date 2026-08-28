/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * program_plan_cache_fixture.c - Shared program TargetPlan cache test authority
 */

#include "program_plan_cache_fixture.h"

#include "base/xfileio.h"
#include "base/xmalloc.h"
#include "ir/xi.h"
#include "ir/xi_module.h"
#include "os/os_dir.h"
#include "os/os_fs.h"
#include "plan/format/xr_xtp_schema.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/target/xr_target_plan.h"
#include "runtime/value/xtype.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* The budget selects the deterministic baseline plan. Every suite that shares
 * this fixture must share it, or two suites would key the same authority to
 * two different cache objects and prove nothing about each other. */
static const uint8_t k_optimization_policy[] = "program-target-plan-build-kat-v1";

static XrType g_program_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static const char k_program_module_identity[] =
    "memory-module-v1:id=36:program-target-plan-build-fixture-v1";

XrSemanticPlan *xr_test_program_plan_semantic(uint32_t ordinal) {
    char name[48];
    (void) snprintf(name, sizeof(name), "program_target_%u", ordinal);
    XiFunc *function = xi_func_new(name, &g_program_int);
    if (!function)
        return NULL;
    function->module = xi_module_new("fixture/program_target_plan.xr", name, function);
    if (!function->module || !xi_module_set_identity(function->module, k_program_module_identity)) {
        xi_func_free(function);
        return NULL;
    }
    XiBlock *entry = xi_block_new(function);
    XiValue *constant = entry ? xi_const_int(function, entry, ordinal, &g_program_int) : NULL;
    if (!entry || !constant) {
        xi_func_free(function);
        return NULL;
    }
    xi_block_set_return(entry, constant);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    xi_func_free(function);
    return built ? semantic : NULL;
}

XrCacheStore *xr_test_program_plan_store_open(const char *root, uint64_t quota_bytes,
                                              size_t max_entry_bytes) {
    XrCacheStoreConfig config = {
        .root = root,
        .quota_bytes = quota_bytes,
        .max_entry_bytes = max_entry_bytes,
        .stale_temp_age_ns = UINT64_C(1000000000),
    };
    return xr_cache_store_open(&config);
}

static void clear_directory(const char *path) {
    XrDirIter *iterator = xr_dir_open(path);
    if (iterator) {
        XrDirEntry entry;
        while (xr_dir_next(iterator, &entry)) {
            if (entry.is_dir)
                continue;
            char *child = xr_path_join(path, entry.name);
            if (child) {
                (void) xr_fs_remove(child);
                xr_free(child);
            }
        }
        xr_dir_close(iterator);
    }
    (void) xr_test_rmdir(path);
}

void xr_test_program_plan_store_remove(const char *root) {
    char *xsm = xr_path_join(root, "xsm");
    char *xtp = xr_path_join(root, "xtp");
    if (xsm)
        clear_directory(xsm);
    if (xtp)
        clear_directory(xtp);
    xr_free(xsm);
    xr_free(xtp);
    clear_directory(root);
}

/* A name the store has not finished publishing. Counting these separately is
 * what proves a crashed or cancelled writer never left a readable half-object. */
static bool is_temp_name(const char *name) {
    return strncmp(name, ".tmp-", 5u) == 0;
}

static void inventory_directory(const char *path, XrTestProgramPlanStoreInventory *out) {
    XrDirIter *iterator = xr_dir_open(path);
    if (!iterator)
        return;
    XrDirEntry entry;
    while (xr_dir_next(iterator, &entry)) {
        if (entry.is_dir)
            continue;
        if (is_temp_name(entry.name)) {
            out->temps++;
            continue;
        }
        out->objects++;
        char *child = xr_path_join(path, entry.name);
        XrFsStat stat;
        if (child && xr_fs_stat(child, &stat) == 0)
            out->object_bytes += stat.size;
        xr_free(child);
    }
    xr_dir_close(iterator);
}

void xr_test_program_plan_store_inventory(const char *root, XrTestProgramPlanStoreInventory *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    static const char *const kinds[] = {"xsm", "xtp"};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        char *directory = xr_path_join(root, kinds[i]);
        if (!directory)
            continue;
        inventory_directory(directory, out);
        xr_free(directory);
    }
}

bool xr_test_program_plan_build(const XrTestProgramPlanBuildInput *input,
                                XrProgramTargetPlanBuildResult *result, char *error,
                                size_t error_size) {
    XrCacheFingerprint budget;
    xr_cache_fingerprint_bytes(k_optimization_policy, sizeof(k_optimization_policy) - 1u, &budget);
    XrProgramTargetPlanBuildRequest request = {
        .semantic_plan = input->semantic,
        .semantic_dependencies = input->dependencies,
        .semantic_dependency_count = input->dependency_count,
        .profile = input->profile,
        .cache_store = input->store,
        .optimization_budget = budget,
        .rebuild = input->rebuild,
        .cancellation = input->cancellation,
    };
    return xr_program_target_plan_build(&request, result, error, error_size);
}

bool xr_test_program_plan_encode(const XrTargetPlan *plan, uint8_t **bytes, size_t *size) {
    char error[512] = {0};
    return xr_xtp_encode_plan(plan, bytes, size, error, sizeof(error));
}

void xr_test_program_plan_encoded_free(uint8_t *bytes) {
    xr_xtp_encoded_free(bytes);
}
