/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_runtime_allocations.h - Count physical runtime ownership for real source
 *
 * KEY CONCEPT:
 *   Generated native and VM entries use the actual runtime with allocation faults.
 */
#ifndef XIR_SOURCE_RUNTIME_ALLOCATIONS_H
#define XIR_SOURCE_RUNTIME_ALLOCATIONS_H
#include "base/xmalloc.h"
#include "xir/xxir_program.h"
static size_t runtime_attempts, runtime_fail_at = SIZE_MAX, runtime_live, runtime_bytes;
static struct { void *pointer; size_t bytes; } runtime_owned[8192];
static void runtime_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(runtime_live < 8192 && bytes <= SIZE_MAX - runtime_bytes);
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
#include "xir/xxir_value.c"
#include "xir/xxir_scalar.c"
#include "xir/xxir_call.c"
#include "xir/xxir_program.c"
#include "xir/xxir_instance.c"
#include "xir/xxir_output.c"

static bool runtime_sink(void *context, const XrXirOutputGroup *group) {
    (void) context;
    CHECK(group && group->count <= 8); return true;
}
static XrXirCallStatus runtime_drive(XrXirInstance *instance) {
    XrXirInstanceResult result = xr_xir_instance_poll(instance);
    unsigned wakes = 0;
    while (result.outcome.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(++wakes <= 2);
        CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_READY);
        result = xr_xir_instance_poll(instance);
    }
    return result.outcome.status;
}
static void runtime_source_attempt(XrXirProgram *program, uint32_t entry, uint32_t resume_text) {
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    config.output = (XrXirOutputProvider) {runtime_sink, NULL};
    XrXirInstance *instance = NULL; XrXirValue result = {0}, argument = {XR_XIR_BOOL, 0, 1};
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
    if (status == XR_XIR_CALL_RETURNED) status = xr_xir_instance_start(instance, resume_text, &argument, 1);
    if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
    if (status == XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    else CHECK(runtime_fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    if (status == XR_XIR_CALL_RETURNED) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 3 && !memcmp(bytes, "ry!", 3));
        xr_xir_value_drop(&result);
    }
}
static void runtime_source_failures(XrXirProgram *program, uint32_t entry, uint32_t resume_text) {
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        runtime_source_attempt(program, entry, resume_text);
        if (!attempt) sites = runtime_attempts;
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Real source runtime physical release: %zu allocation failure sites\n", sites);
}
#endif
