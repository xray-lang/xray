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
static size_t runtime_attempts, runtime_fail_at = SIZE_MAX, runtime_live, runtime_bytes;
static struct { void *pointer; size_t bytes; } runtime_owned[16384];
static void runtime_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(runtime_live < sizeof(runtime_owned) / sizeof(runtime_owned[0]) && bytes <= SIZE_MAX - runtime_bytes);
    runtime_owned[runtime_live].pointer = pointer;
    runtime_owned[runtime_live++].bytes = bytes; runtime_bytes += bytes;
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
    for (size_t i = 0; i < runtime_live; ++i) if (runtime_owned[i].pointer == pointer) {
        runtime_bytes -= runtime_owned[i].bytes; runtime_owned[i] = runtime_owned[--runtime_live]; break;
    }
    xr_free(pointer);
}
static void *runtime_realloc(void *pointer, size_t size) {
    if (runtime_attempts++ == runtime_fail_at) return NULL;
    size_t index = 0;
    while (index < runtime_live && runtime_owned[index].pointer != pointer) ++index;
    void *replacement = xr_realloc(pointer, size);
    if (replacement) {
        if (index < runtime_live) {
            runtime_bytes -= runtime_owned[index].bytes;
            runtime_owned[index].pointer = replacement; runtime_owned[index].bytes = size; runtime_bytes += size;
        } else runtime_record(replacement, size);
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
#include "xir/xxir_verify.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_program_match.c"
#include "xir/xxir_program.c"
#include "xir/xxir_instance.c"
#include "xir/xxir_output.c"
#endif // XIR_RUNTIME_ALLOCATIONS_H
