/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_allocations.c - Xi construction allocation failure atomicity
 */

#include "ir/xi.h"
#include "runtime/value/xtype.h"
#include "../program/xr_program_allocation_probe.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static size_t attempts, fail_at, live_count;
static void *live[128];

static void *track(void *pointer) {
    REQUIRE(pointer != NULL);
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i])
            continue;
        live[i] = pointer;
        live_count++;
        return pointer;
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_malloc(size));
}

void *xr_program_test_calloc(size_t count, size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_calloc(count, size));
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (++attempts == fail_at)
        return NULL;
    if (!pointer)
        return track(xr_program_test_system_realloc(NULL, size));
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i] != pointer)
            continue;
        void *grown = xr_program_test_system_realloc(pointer, size);
        REQUIRE(grown != NULL);
        live[i] = grown;
        return grown;
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i] != pointer)
            continue;
        live[i] = NULL;
        live_count--;
        xr_program_test_system_free(pointer);
        return;
    }
    abort();
}

static XrType unit_type = {.kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE};

static void leave_arena_space(XiFunc *function, uint32_t remaining) {
    REQUIRE(remaining < XI_ARENA_INITIAL_SIZE);
    REQUIRE(xi_func_arena_alloc(function, XI_ARENA_INITIAL_SIZE) != NULL);
    if (remaining != 0u)
        REQUIRE(xi_func_arena_alloc(function, XI_ARENA_INITIAL_SIZE - remaining) != NULL);
}

static void fail_next(void) {
    attempts = 0u;
    fail_at = 1u;
}

static void finish(XiFunc *function) {
    REQUIRE(attempts >= fail_at);
    fail_at = 0u;
    xi_func_free(function);
    REQUIRE(live_count == 0u);
}

static void test_block_allocation(uint32_t remaining) {
    XiFunc *function = xi_func_new("allocation", &unit_type);
    REQUIRE(function != NULL);
    leave_arena_space(function, remaining);
    fail_next();
    REQUIRE(xi_block_new(function) == NULL);
    REQUIRE(function->nblocks == 0u && function->entry == NULL);
    finish(function);
}

static void test_value_allocation(uint32_t remaining, bool grow_values) {
    XiFunc *function = xi_func_new("allocation", &unit_type);
    REQUIRE(function != NULL);
    XiBlock *block = xi_block_new(function);
    REQUIRE(block != NULL);
    if (grow_values) {
        uint32_t capacity = block->values_cap;
        for (uint32_t i = 0u; i < capacity; ++i)
            REQUIRE(xi_value_new(function, block, XI_CONST, &unit_type, 0u) != NULL);
    }
    uint32_t before = block->nvalues;
    leave_arena_space(function, remaining);
    fail_next();
    REQUIRE(xi_value_new(function, block, XI_THROW, &unit_type, 1u) == NULL);
    REQUIRE(block->nvalues == before);
    finish(function);
}

static void test_block_index_allocation(void) {
    XiFunc *function = xi_func_new("allocation", &unit_type);
    REQUIRE(function != NULL);
    for (uint32_t i = 0u; i < 16u; ++i)
        REQUIRE(xi_block_new(function) != NULL);
    REQUIRE(function->nblocks == function->blocks_cap);
    XiBlock *entry = function->entry;
    fail_next();
    REQUIRE(xi_block_new(function) == NULL);
    REQUIRE(function->nblocks == 16u && function->entry == entry);
    finish(function);
}

static void test_predecessor_allocation(void) {
    XiFunc *function = xi_func_new("allocation", &unit_type);
    REQUIRE(function != NULL);
    XiBlock *block = xi_block_new(function);
    REQUIRE(block != NULL);
    uint16_t before = block->preds_cap;
    for (uint16_t i = 0u; i < before; ++i)
        REQUIRE(xi_block_add_pred(block, block));
    XiBlock **original = block->preds;
    leave_arena_space(function, 0u);
    fail_next();
    REQUIRE(!xi_block_add_pred(block, block));
    REQUIRE(block->npreds == before && block->preds == original && block->preds_cap == before);
    for (uint16_t i = 0u; i < before; ++i)
        REQUIRE(block->preds[i] == block);
    REQUIRE(attempts == 1u);
    fail_at = 0u;
    REQUIRE(xi_block_add_pred(block, block));
    REQUIRE(block->npreds == before + 1u && block->preds[before] == block);
    finish(function);
}

int main(void) {
    test_block_allocation(0u);
    test_block_allocation(((uint32_t) sizeof(XiBlock) + 7u) & ~7u);
    test_block_allocation((((uint32_t) sizeof(XiBlock) + 7u) & ~7u) +
                          16u * (uint32_t) sizeof(XiValue *));
    test_value_allocation(0u, false);
    test_value_allocation(((uint32_t) sizeof(XiValue) + 7u) & ~7u, false);
    test_value_allocation(0u, true);
    test_block_index_allocation();
    test_predecessor_allocation();
    puts("Xi allocation failures: 8 construction boundaries passed, no published partial object or leak");
    return 0;
}
