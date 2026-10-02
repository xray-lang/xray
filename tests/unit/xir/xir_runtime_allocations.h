/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_runtime_allocations.h - Count actual runtime allocation and physical ownership
 *
 * KEY CONCEPT:
 *   VM and generated native entries share fault-injected runtime implementations.
 */
#ifndef XIR_RUNTIME_ALLOCATIONS_H
#define XIR_RUNTIME_ALLOCATIONS_H
#include "base/xmalloc.h"
#include "xir/xxir_program.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
static size_t runtime_attempts, runtime_fail_at = SIZE_MAX, runtime_live, runtime_bytes;
/* Live runtime blocks, open-addressed by pointer with linear probing. Fault
 * injection runs every program once per allocation site, so the bookkeeping
 * must stay O(1) per call: a linear scan of the live set made each run
 * quadratic in its live allocations and dominated the whole test. */
typedef struct RuntimeAllocationRecord { void *pointer; size_t bytes; } RuntimeAllocationRecord;
static RuntimeAllocationRecord *runtime_owned;
static size_t runtime_owned_capacity; /* zero or a power of two */
static size_t runtime_slot(const void *pointer) {
    uint64_t key = (uint64_t) (uintptr_t) pointer;
    key ^= key >> 33; key *= 0xff51afd7ed558ccdULL; key ^= key >> 33;
    return (size_t) key & (runtime_owned_capacity - 1);
}
static void runtime_place(void *pointer, size_t bytes) {
    size_t slot = runtime_slot(pointer);
    while (runtime_owned[slot].pointer) slot = (slot + 1) & (runtime_owned_capacity - 1);
    runtime_owned[slot].pointer = pointer; runtime_owned[slot].bytes = bytes;
}
static void runtime_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(bytes <= SIZE_MAX - runtime_bytes);
    /* Bookkeeping is outside the runtime injection boundary. Every runtime
     * allocation still receives its original fault index and physical count. */
    if (runtime_live + 1 > runtime_owned_capacity / 2) {
        size_t capacity = runtime_owned_capacity ? runtime_owned_capacity * 2 : 512;
        CHECK(capacity <= SIZE_MAX / sizeof(*runtime_owned));
        RuntimeAllocationRecord *previous = runtime_owned;
        size_t previous_capacity = runtime_owned_capacity;
        runtime_owned = xr_calloc(capacity, sizeof(*runtime_owned)); CHECK(runtime_owned);
        runtime_owned_capacity = capacity;
        for (size_t i = 0; i < previous_capacity; ++i)
            if (previous[i].pointer) runtime_place(previous[i].pointer, previous[i].bytes);
        xr_free(previous);
    }
    runtime_place(pointer, bytes);
    ++runtime_live; runtime_bytes += bytes;
}
/* Backward-shift deletion keeps every probe chain contiguous without tombstones. */
static bool runtime_forget(const void *pointer) {
    if (!pointer || !runtime_live) return false;
    size_t mask = runtime_owned_capacity - 1, hole = runtime_slot(pointer);
    while (runtime_owned[hole].pointer != pointer) {
        if (!runtime_owned[hole].pointer) return false;
        hole = (hole + 1) & mask;
    }
    runtime_bytes -= runtime_owned[hole].bytes; --runtime_live;
    for (size_t next = (hole + 1) & mask; runtime_owned[next].pointer; next = (next + 1) & mask) {
        size_t home = runtime_slot(runtime_owned[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { runtime_owned[hole] = runtime_owned[next]; hole = next; }
    }
    runtime_owned[hole].pointer = NULL; runtime_owned[hole].bytes = 0;
    return true;
}
static inline void runtime_report_residuals(FILE *stream) {
    for (size_t i = 0, index = 0; i < runtime_owned_capacity; ++i)
        if (runtime_owned[i].pointer)
            fprintf(stream, "residual %zu: %zu bytes\n", index++, runtime_owned[i].bytes);
}
static void *runtime_malloc(size_t size) {
    if (runtime_attempts++ == runtime_fail_at) return NULL;
    void *pointer = xr_malloc(size); runtime_record(pointer, size); return pointer;
}
static void *runtime_calloc(size_t count, size_t size) {
    if (runtime_attempts++ == runtime_fail_at) return NULL;
    CHECK(!size || count <= SIZE_MAX / size);
    void *pointer = xr_calloc(count, size); runtime_record(pointer, count * size); return pointer;
}
static void runtime_free(void *pointer) {
    (void) runtime_forget(pointer);
    xr_free(pointer);
    if (!runtime_live) {
        xr_free(runtime_owned); runtime_owned = NULL; runtime_owned_capacity = 0;
    }
}
static void *runtime_realloc(void *pointer, size_t size) {
    if (runtime_attempts++ == runtime_fail_at) return NULL;
    void *replacement = xr_realloc(pointer, size);
    if (replacement) {
        (void) runtime_forget(pointer);
        runtime_record(replacement, size);
    }
    return replacement;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) runtime_malloc(size)
#define xr_calloc(count, size) runtime_calloc(count, size)
#define xr_realloc(pointer, size) runtime_realloc(pointer, size)
#define xr_free(pointer) runtime_free(pointer)
#include "xir/xxir_declarations.c"
#include "xir/xxir_types.c"
#include "xir/xxir_constraints.c"
#include "xir/xxir_constraint_proof.c"
#include "xir/xxir_implementation.c"
#include "xir/xxir_implementation_verify.c"
#include "xir/xxir_interface.c"
#include "xir/xxir_interface_members.c"
#include "xir/xxir_type_layout.c"
#include "xir/xxir_type_arena.c"
#include "xir/xxir_value.c"
#include "xir/xxir_scalar.c"
#include "xir/xxir_float.c"
#include "xir/xxir_call.c"
#include "xir/xxir_generic.c"
#include "xir/xxir.c"
#include "xir/xxir_checked.c"
#include "xir/xxir_defaults.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_program_match.c"
#include "xir/xxir_program.c"
#include "xir/xxir_instance.c"
#include "xir/xxir_output.c"
#endif // XIR_RUNTIME_ALLOCATIONS_H
