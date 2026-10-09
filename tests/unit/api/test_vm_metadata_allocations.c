/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_vm_metadata_allocations.c - Runtime metadata allocation qualification
 *
 * KEY CONCEPT:
 *   Real VM initialization forwards metadata operations and observes their owned allocations.
 */
#include "xr_vm_metadata_probe.h"
#include "runtime/xisolate_internal.h"
#include "runtime/xisolate_api.h"
#include "runtime/core/xr_exec_context.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "stage=%d ordinal=%zu attempts=%zu/%zu/%zu rejects=%zu core=%zu line=%d: %s\n", \
        (int)chosen.stage, chosen.ordinal, attempts[VM_META_CREATE], attempts[VM_META_BUILTINS], \
        attempts[VM_META_REGISTRY], rejected, core_calls, __LINE__, #condition); exit(1); \
} } while (0)
_Static_assert(SYMBOL_BUILTIN_COUNT == 255, "Review fixed builtin map allocation plan");
typedef enum Stage { VM_META_NONE, VM_META_CREATE, VM_META_BUILTINS, VM_META_REGISTRY, VM_META_STAGE_COUNT } Stage;
typedef struct Failure { Stage stage; size_t ordinal; } Failure;
typedef struct Expected { size_t bytes; const char *source; } Expected;
typedef struct Allocation { void *memory; size_t bytes; Failure site; } Allocation;
typedef union MapOwnerModel {
    XrOsIoPolicy policy;
    long double alignment;
    void *pointer;
    uint64_t integer;
} MapOwnerModel;
static const Expected create_plan[] = {
    {sizeof(XrSymbolTable), "xsymbol_table.c"},
    {sizeof(MapOwnerModel) + sizeof(XrHashMap), "xio_policy.c"},
    {16 * sizeof(XrHashMapEntry), "xio_policy.c"},
    {256 * sizeof(const char *), "xsymbol_table.c"}
};
static const Expected builtin_plan[] = {
    {sizeof(MapOwnerModel) + sizeof(XrHashMap), "xio_policy.c"},
    {16 * sizeof(XrHashMapEntry), "xio_policy.c"},
    {32 * sizeof(XrHashMapEntry), "xio_policy.c"},
    {64 * sizeof(XrHashMapEntry), "xio_policy.c"},
    {128 * sizeof(XrHashMapEntry), "xio_policy.c"},
    {256 * sizeof(XrHashMapEntry), "xio_policy.c"},
    {512 * sizeof(XrHashMapEntry), "xio_policy.c"}
};
static const Expected registry_plan[] = {
    {sizeof(XrTypeRegistry), "xtype_registry.c"},
    {64 * sizeof(XrTypeMetadata *), "xtype_registry.c"},
    {sizeof(MapOwnerModel) + sizeof(XrHashMap), "xio_policy.c"},
    {16 * sizeof(XrHashMapEntry), "xio_policy.c"}
};
static const Failure cases[] = {
    {VM_META_CREATE,1}, {VM_META_CREATE,2}, {VM_META_CREATE,3}, {VM_META_CREATE,4},
    {VM_META_BUILTINS,1}, {VM_META_BUILTINS,2}, {VM_META_BUILTINS,3}, {VM_META_BUILTINS,4},
    {VM_META_BUILTINS,5}, {VM_META_BUILTINS,6}, {VM_META_BUILTINS,7},
    {VM_META_REGISTRY,1}, {VM_META_REGISTRY,2}, {VM_META_REGISTRY,3}, {VM_META_REGISTRY,4}
};
static XR_THREAD_LOCAL Stage active_stage;
static atomic_flag observer_lock = ATOMIC_FLAG_INIT;
static Allocation live[32];
static Failure released[32];
static size_t blocks, bytes_live, release_count, rejected;
static size_t attempts[VM_META_STAGE_COUNT], allocations[VM_META_STAGE_COUNT], frees[VM_META_STAGE_COUNT];
static size_t entered[VM_META_STAGE_COUNT], core_calls;
static bool result_ok[VM_META_STAGE_COUNT];
static Failure chosen;
static XrVMRuntime *entered_runtime;

static void lock_observer(void) {
    while (atomic_flag_test_and_set_explicit(&observer_lock, memory_order_acquire)) { }
}
static void unlock_observer(void) {
    atomic_flag_clear_explicit(&observer_lock, memory_order_release);
}
static const char *basename_of(const char *source) {
    const char *name = source;
    for (; *source; ++source) if (*source == '/' || *source == '\\') name = source + 1;
    return name;
}
static size_t slot_for(void *memory) {
    for (size_t i = 0; i < 32; ++i) if (live[i].memory == memory) return i;
    return 32;
}
static Expected expected_at(Stage stage, size_t ordinal) {
    CHECK(ordinal > 0);
    switch (stage) {
    case VM_META_CREATE: CHECK(ordinal <= 4); return create_plan[ordinal - 1];
    case VM_META_BUILTINS: CHECK(ordinal <= 7); return builtin_plan[ordinal - 1];
    case VM_META_REGISTRY: CHECK(ordinal <= 4); return registry_plan[ordinal - 1];
    default: fprintf(stderr, "invalid allocation stage\n"); exit(1);
    }
}
XR_FUNC void *xr_vm_metadata_test_malloc(size_t bytes, const char *source) {
    Stage stage = active_stage;
    if (stage == VM_META_NONE) return xr_malloc(bytes);
    lock_observer();
    size_t ordinal = ++attempts[stage];
    Expected expected = expected_at(stage, ordinal);
    CHECK(bytes == expected.bytes && !strcmp(basename_of(source), expected.source));
    bool reject = stage == chosen.stage && ordinal == chosen.ordinal;
    if (reject) ++rejected;
    unlock_observer();
    if (reject) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory != NULL);
    lock_observer();
    size_t slot = slot_for(NULL);
    CHECK(slot < 32 && blocks < 32 && bytes_live <= SIZE_MAX - bytes);
    live[slot] = (Allocation){memory, bytes, {stage, ordinal}};
    ++blocks; bytes_live += bytes; ++allocations[stage];
    unlock_observer();
    return memory;
}
XR_FUNC void *xr_vm_metadata_test_calloc(size_t count, size_t bytes) {
    CHECK(active_stage == VM_META_NONE);
    return xr_calloc(count, bytes);
}
XR_FUNC void *xr_vm_metadata_test_realloc(void *memory, size_t bytes) {
    CHECK(active_stage == VM_META_NONE);
    lock_observer(); bool tracked = memory && slot_for(memory) < 32; unlock_observer();
    CHECK(!tracked);
    return xr_realloc(memory, bytes);
}
XR_FUNC void xr_vm_metadata_test_free(void *memory) {
    if (!memory) return;
    lock_observer();
    size_t slot = slot_for(memory);
    if (slot < 32) {
        CHECK(blocks && bytes_live >= live[slot].bytes && release_count < 32);
        released[release_count++] = live[slot].site;
        ++frees[live[slot].site.stage]; --blocks; bytes_live -= live[slot].bytes;
        live[slot] = (Allocation){0};
    }
    unlock_observer();
    // Untagged compiler/runtime owners still use their real release path.
    xr_free(memory);
}
static void enter_stage(Stage stage) {
    CHECK(active_stage == VM_META_NONE && stage > VM_META_NONE && stage < VM_META_STAGE_COUNT);
    CHECK(g_current_isolate != NULL && xr_exec_context_current() != NULL);
    if (stage == VM_META_CREATE) entered_runtime = g_current_isolate;
    CHECK(g_current_isolate == entered_runtime);
    ++entered[stage]; active_stage = stage;
}
static void leave_stage(Stage stage, bool ok) {
    CHECK(active_stage == stage); result_ok[stage] = ok; active_stage = VM_META_NONE;
}
XR_FUNC XrSymbolTable *xr_vm_metadata_test_symbol_create(void) {
    enter_stage(VM_META_CREATE);
    XrSymbolTable *table = xr_symbol_table_create();
    leave_stage(VM_META_CREATE, table != NULL);
    return table;
}
XR_FUNC bool xr_vm_metadata_test_symbol_builtins(XrSymbolTable *table) {
    enter_stage(VM_META_BUILTINS);
    bool ok = xr_symbol_table_init_builtins(table);
    leave_stage(VM_META_BUILTINS, ok);
    return ok;
}
XR_FUNC void xr_vm_metadata_test_registry_init(XrVMRuntime *runtime) {
    enter_stage(VM_META_REGISTRY); CHECK(runtime == entered_runtime);
    xr_registry_init(runtime);
    leave_stage(VM_META_REGISTRY, xr_isolate_get_type_registry(runtime) != NULL);
}
XR_FUNC void xr_vm_metadata_test_core_init(XrVMRuntime *runtime) {
    ++core_calls;
    xr_core_init(runtime);
}
static void physically_empty(void) {
    lock_observer();
    CHECK(!blocks && !bytes_live);
    for (size_t i = 0; i < 32; ++i) CHECK(!live[i].memory);
    unlock_observer();
}
static void reset(Failure failure) {
    physically_empty(); CHECK(active_stage == VM_META_NONE);
    memset(attempts, 0, sizeof(attempts)); memset(allocations, 0, sizeof(allocations));
    memset(frees, 0, sizeof(frees)); memset(entered, 0, sizeof(entered));
    memset(result_ok, 0, sizeof(result_ok)); memset(released, 0, sizeof(released));
    release_count = rejected = core_calls = 0; entered_runtime = NULL; chosen = failure;
}
static void expect_release(size_t *cursor, Stage stage, size_t ordinal) {
    CHECK(*cursor < release_count);
    CHECK(released[*cursor].stage == stage && released[*cursor].ordinal == ordinal);
    ++*cursor;
}
static void fixed_release_order(Failure failure) {
    size_t cursor = 0;
    if (failure.stage == VM_META_CREATE) {
        for (size_t ordinal = failure.ordinal - 1; ordinal > 0; --ordinal)
            expect_release(&cursor, VM_META_CREATE, ordinal);
    } else if (failure.stage == VM_META_BUILTINS) {
        for (size_t ordinal = 2; ordinal < failure.ordinal; ++ordinal)
            expect_release(&cursor, VM_META_BUILTINS, ordinal);
        if (failure.ordinal > 1) expect_release(&cursor, VM_META_BUILTINS, 1);
        expect_release(&cursor, VM_META_CREATE, 3); expect_release(&cursor, VM_META_CREATE, 2);
        expect_release(&cursor, VM_META_CREATE, 4); expect_release(&cursor, VM_META_CREATE, 1);
    } else {
        for (size_t ordinal = 2; ordinal <= 6; ++ordinal)
            expect_release(&cursor, VM_META_BUILTINS, ordinal);
        expect_release(&cursor, VM_META_CREATE, 3); expect_release(&cursor, VM_META_CREATE, 2);
        for (size_t ordinal = failure.ordinal - 1; ordinal > 0; --ordinal)
            expect_release(&cursor, VM_META_REGISTRY, ordinal);
        expect_release(&cursor, VM_META_BUILTINS, 7); expect_release(&cursor, VM_META_BUILTINS, 1);
        expect_release(&cursor, VM_META_CREATE, 4); expect_release(&cursor, VM_META_CREATE, 1);
    }
    CHECK(cursor == release_count);
}
static void one_case(Failure failure) {
    reset(failure);
    XrVMRuntime *prior_isolate = g_current_isolate;
    XrExecutionContext *prior_exec = xr_exec_context_current();
    CHECK(!prior_isolate && !prior_exec);
    XrVMConfig config = {0};
    XrVMRuntime *runtime = xray_vm_new_full(&config);
    CHECK(!runtime && rejected == 1 && !core_calls && active_stage == VM_META_NONE);
    CHECK(g_current_isolate == prior_isolate && xr_exec_context_current() == prior_exec);
    for (Stage stage = VM_META_CREATE; stage < VM_META_STAGE_COUNT; stage = (Stage)(stage + 1)) {
        size_t successful_attempts = stage == VM_META_BUILTINS ? 7u : 4u;
        if (stage < failure.stage) {
            CHECK(entered[stage] == 1 && result_ok[stage] && attempts[stage] == successful_attempts);
            CHECK(allocations[stage] == successful_attempts && frees[stage] == successful_attempts);
        } else if (stage == failure.stage) {
            CHECK(entered[stage] == 1 && !result_ok[stage] && attempts[stage] == failure.ordinal);
            CHECK(allocations[stage] == failure.ordinal - 1 && frees[stage] == failure.ordinal - 1);
        } else {
            CHECK(!entered[stage] && !result_ok[stage] && !attempts[stage] &&
                !allocations[stage] && !frees[stage]);
        }
    }
    fixed_release_order(failure); physically_empty();
}
int main(void) {
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) one_case(cases[i]);
    puts("whole VM metadata: 15 real OOMs return NULL, restore context and release tagged owners");
    return 0;
}
