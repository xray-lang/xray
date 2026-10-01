/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_repl_publication_allocations.c - Atomic typed metadata publication
 */
#include "../test_framework.h"
#include "xray.h"
#include "xray_vm.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/api/xrepl.h"
#include "../../../src/api/xisolate_profile.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../src/module/xmodule_identity.h"
#include "../../../src/base/xmalloc.h"
#include <string.h>
#include "../../../src/base/xchecks.h"

typedef struct ReplProbeAllocation {
    void *pointer;
    size_t bytes;
} ReplProbeAllocation;

static ReplProbeAllocation allocations[128];
static size_t allocation_count;
static size_t live_bytes;
static size_t attempts;
static size_t failure_at;
static bool armed;

static size_t find_allocation(void *pointer) {
    for (size_t i = 0; i < allocation_count; ++i)
        if (allocations[i].pointer == pointer)
            return i;
    return SIZE_MAX;
}

static bool refuse_allocation(void) {
    return armed && ++attempts == failure_at;
}

static void record_allocation(void *pointer, size_t bytes) {
    if (!pointer)
        return;
    XR_CHECK(allocation_count < 128, "REPL allocation probe capacity exhausted");
    allocations[allocation_count++] = (ReplProbeAllocation) {pointer, bytes};
    live_bytes += bytes;
}

XR_FUNC void *xr_repl_probe_malloc(size_t size) {
    if (refuse_allocation())
        return NULL;
    void *pointer = xr_malloc(size);
    if (armed)
        record_allocation(pointer, size);
    return pointer;
}

XR_FUNC void *xr_repl_probe_calloc(size_t count, size_t size) {
    if (refuse_allocation())
        return NULL;
    void *pointer = xr_calloc(count, size);
    if (armed)
        record_allocation(pointer, count * size);
    return pointer;
}

XR_FUNC void *xr_repl_probe_realloc(void *pointer, size_t size) {
    if (refuse_allocation())
        return NULL;
    size_t index = pointer ? find_allocation(pointer) : SIZE_MAX;
    void *next = xr_realloc(pointer, size);
    if (next && index != SIZE_MAX) {
        live_bytes -= allocations[index].bytes;
        allocations[index] = (ReplProbeAllocation) {next, size};
        live_bytes += size;
    }
    /* Shared compiler buffers are released by their own coordinator. Only
     * publication arrays allocated here contribute to this live-byte proof. */
    return next;
}

XR_FUNC void xr_repl_probe_free(void *pointer) {
    size_t index = pointer ? find_allocation(pointer) : SIZE_MAX;
    if (index != SIZE_MAX) {
        live_bytes -= allocations[index].bytes;
        allocations[index] = allocations[--allocation_count];
    }
    xr_free(pointer);
}

static const XrModuleIdentityAuthority authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "repl-publication-allocations",
};

static void exercise_publication(size_t fail, size_t *allocation_attempts) {
    XrVMRuntime *vm = xr_isolate_profile_new(XR_ISOLATE_PROFILE_REPL);
    ASSERT_NOT_NULL(vm);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(vm);
    XrReplEvalResult first =
        xr_repl_eval(session, vm, "fn value() -> i64 { return 5 }\n11 + 0\n", &authority);
    ASSERT_EQ(first.status, XR_REPL_EVAL_OK);
    ASSERT_NOT_NULL(first.proto);
    XrReplSymbolTable *table = xr_repl_symbols_of(vm);
    XrReplSymbolTable before = *table;
    XrReplSymbol symbols[32];
    XrReplSymbol results[32];
    ASSERT_LE(table->count, 32);
    ASSERT_LE(table->result_count, 32);
    memcpy(symbols, table->symbols, (size_t) table->count * sizeof(*symbols));
    memcpy(results, table->results, (size_t) table->result_count * sizeof(*results));
    attempts = 0;
    failure_at = fail;
    armed = true;
    XrReplEvalResult next = xr_repl_eval(
        session, vm, "fn value() -> string { return \"later\" }\nvar added = 9\n7 + 8\n",
        &authority);
    armed = false;
    size_t count = attempts;
    if (fail) {
        ASSERT_EQ(next.status, XR_REPL_EVAL_COMPILE_ERROR);
        ASSERT_NULL(next.proto);
        ASSERT_EQ_INT(live_bytes, 0);
        ASSERT_EQ_INT(allocation_count, 0);
        ASSERT_EQ_INT(table->count, before.count);
        ASSERT_EQ_INT(table->result_count, before.result_count);
        ASSERT_TRUE(table->symbols == before.symbols);
        ASSERT_TRUE(table->results == before.results);
        ASSERT_TRUE(table->latest_result_name == before.latest_result_name);
        ASSERT_EQ_INT(memcmp(table->symbols, symbols, (size_t) before.count * sizeof(*symbols)), 0);
        ASSERT_EQ_INT(
            memcmp(table->results, results, (size_t) before.result_count * sizeof(*results)), 0);
        XrCompilerSessionReplGenerationSnapshot ledger =
            xr_compiler_session_repl_generation_snapshot(session);
        ASSERT_EQ_INT(ledger.published_count, 1);
        ASSERT_EQ_INT(ledger.abandoned_count, 1);
        int64_t last = 0;
        ASSERT_TRUE(xr_repl_peek_int(vm, "it", &last));
        ASSERT_EQ_INT(last, 11);
        XrReplEvalResult recovery =
            xr_repl_eval(session, vm, "var recovered = value()\n", &authority);
        ASSERT_EQ(recovery.status, XR_REPL_EVAL_OK);
        ASSERT_NOT_NULL(recovery.proto);
        int64_t value = 0;
        ASSERT_TRUE(xr_repl_peek_int(vm, "recovered", &value));
        ASSERT_EQ_INT(value, 5);
        xr_free_code(vm, recovery.proto);
    } else {
        ASSERT_EQ(next.status, XR_REPL_EVAL_OK);
        ASSERT_NOT_NULL(next.proto);
        int64_t last = 0;
        ASSERT_TRUE(xr_repl_peek_int(vm, "it", &last));
        ASSERT_EQ_INT(last, 15);
    }
    if (next.proto)
        xr_free_code(vm, next.proto);
    xr_free_code(vm, first.proto);
    xray_vm_delete(vm);
    ASSERT_EQ_INT(live_bytes, 0);
    ASSERT_EQ_INT(allocation_count, 0);
    *allocation_attempts = count;
}

TEST(repl_actual_coordinator_allocations_do_not_publish_partial_metadata) {
    size_t successful_allocations = 0;
    exercise_publication(0, &successful_allocations);
    ASSERT_GT(successful_allocations, 0);
    for (size_t i = 1; i <= successful_allocations; ++i) {
        size_t actual_attempts = 0;
        exercise_publication(i, &actual_attempts);
    }
    printf("actual REPL coordinator allocations: %zu; publication buffers reclaimed\n",
           successful_allocations);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("REPL publication allocation safety");
RUN_TEST(repl_actual_coordinator_allocations_do_not_publish_partial_metadata);
TEST_MAIN_END()
