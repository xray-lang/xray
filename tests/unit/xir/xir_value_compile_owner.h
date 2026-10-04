/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_value_compile_owner.h - Finite compiler owners independent of runtime domains
 *
 * KEY CONCEPT:
 *   Actual compiler allocations are observed separately from runtime fault sites.
 */
#ifndef XIR_VALUE_COMPILE_OWNER_H
#define XIR_VALUE_COMPILE_OWNER_H
#include "base/xmalloc.h"
#include "xir/xxir_compile_context.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
typedef struct ValueCompileBlock { void *pointer; size_t bytes; } ValueCompileBlock;
static ValueCompileBlock value_compile_blocks[65536];
static size_t value_compile_calls, value_compile_live, value_compile_bytes;
static size_t value_compile_fail_at = SIZE_MAX;
static bool value_compile_injected;
static void *value_compile_malloc(size_t bytes) {
    if (value_compile_calls++ == value_compile_fail_at) { value_compile_injected = true; return NULL; }
    void *p = xr_malloc(bytes);
    if (p) {
        CHECK(value_compile_live < 65536 && bytes <= SIZE_MAX - value_compile_bytes);
        value_compile_blocks[value_compile_live++] = (ValueCompileBlock){p, bytes};
        value_compile_bytes += bytes;
    }
    return p;
}
static void value_compile_free(void *p) {
    if (!p) return;
    size_t i = 0;
    while (i < value_compile_live && value_compile_blocks[i].pointer != p) ++i;
    CHECK(i < value_compile_live && value_compile_blocks[i].bytes <= value_compile_bytes);
    value_compile_bytes -= value_compile_blocks[i].bytes;
    value_compile_blocks[i] = value_compile_blocks[--value_compile_live];
    xr_free(p);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) value_compile_malloc(bytes)
#define xr_free(p) value_compile_free(p)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
typedef struct ValueCompileOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
} ValueCompileOwner;
static XrCompileResourceStats value_compile_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats s = {0};
    CHECK(xr_compile_resources_stats(context->resources, &s) == XR_COMPILE_RESOURCE_OK);
    return s;
}
/* Original memory maxima bound owned and temporary storage. Actual zero/copy
 * work was absent from the former step counter; the explicit finite formula
 * includes that byte work. Exact boundaries use the entire real operation. */
static XrCompileResourceLimits value_compile_limits(uint64_t metadata, uint64_t scratch, uint64_t steps) {
    CHECK(metadata <= UINT64_C(16777216) && scratch <= UINT64_C(16777216));
    CHECK(steps <= UINT64_C(16777216));
    uint64_t bytes = metadata + scratch + 4096;
    return (XrCompileResourceLimits){bytes, bytes, steps + 2 * bytes};
}
static XrXirStatus value_compile_owner_new(ValueCompileOwner *owner,
    XrCompileResourceLimits limits, uint32_t parameters) {
    CHECK(!owner->context.resources);
    XrCompileResourceStatus status = xr_compile_resources_new(&limits, &owner->context.resources);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    owner->context.limits = xr_xir_compile_default_limits();
    owner->context.limits.parameters = parameters;
    owner->baseline = value_compile_stats(&owner->context);
    return XR_XIR_OK;
}
static void value_compile_owner_release(ValueCompileOwner *owner) {
    if (owner->context.resources) xr_compile_resources_release(owner->context.resources);
    *owner = (ValueCompileOwner){0};
}
static XrXirValueStatus value_compile_arena(const XrXirTypes *types, uint32_t parameters,
    XrCompileResourceLimits limits, XrXirTypeArena **output) {
    ValueCompileOwner owner = {0};
    XrXirStatus status = value_compile_owner_new(&owner, limits, parameters);
    if (status != XR_XIR_OK) return status == XR_XIR_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT;
    XrXirValueStatus result = xr_xir_compile_type_arena_new(&owner.context, types, output);
    /* The owned arena block pins this exact ledger after producer release. */
    value_compile_owner_release(&owner);
    return result;
}
typedef struct ValueCompileProbe {
    const XrXirTypes *types;
    const XrXirNominalTable *nominals;
    XrXirType type;
    uint32_t operation, parameters;
} ValueCompileProbe;
typedef struct ValueCompileResult {
    XrXirTypeArena *arena;
    XrXirNominalTable *nominals;
    XrXirLayout layout;
    uint32_t offsets[2];
} ValueCompileResult;
static XrXirStatus value_compile_probe(const XrXirCompileContext *c,
    const ValueCompileProbe *probe, ValueCompileResult *result) {
    if (probe->operation == 1)
        return xr_xir_compile_nominal_project(c, probe->nominals, &result->nominals);
    if (probe->operation == 2) {
        const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        return xr_xir_compile_nominal_layout(c, probe->types, probe->type, &target,
            &result->layout, result->offsets, 2);
    }
    if (probe->operation == 3) return xr_xir_compile_class_field_verify(c, probe->types, probe->type);
    XrXirValueStatus status = xr_xir_compile_type_arena_new(c, probe->types, &result->arena);
    return status == XR_XIR_VALUE_OK ? XR_XIR_OK : status == XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
        status == XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
}
static void value_compile_result_free(ValueCompileResult *result) {
    xr_xir_compile_type_arena_drop(result->arena);
    xr_xir_compile_nominal_free(result->nominals);
    *result = (ValueCompileResult){0};
}
/* Replay includes owner creation and never restores a stage snapshot. */
static XrXirStatus value_compile_attempt(const ValueCompileProbe *probe,
    XrCompileResourceLimits limits, XrCompileResourceStats *stats) {
    ValueCompileOwner owner = {0}; ValueCompileResult result = {0};
    XrXirStatus status = value_compile_owner_new(&owner, limits, probe->parameters);
    if (status == XR_XIR_OK) {
        XrXirCompileLimits original = owner.context.limits;
        result.offsets[0] = result.offsets[1] = 99;
        status = value_compile_probe(&owner.context, probe, &result);
        CHECK(!memcmp(&original, &owner.context.limits, sizeof(original)));
        XrCompileResourceStats performed = value_compile_stats(&owner.context);
        if (stats) *stats = performed;
        if (status != XR_XIR_OK) {
            CHECK(!result.arena && !result.nominals);
            if (probe->operation == 2)
                CHECK(!result.layout.size && !result.layout.alignment && result.offsets[0] == 99 && result.offsets[1] == 99);
        }
        value_compile_result_free(&result);
        XrCompileResourceStats released = value_compile_stats(&owner.context);
        CHECK(released.live_bytes == owner.baseline.live_bytes);
        CHECK(released.work == performed.work && released.allocated_bytes == performed.allocated_bytes);
    }
    value_compile_owner_release(&owner);
    return status;
}
static void value_compile_boundaries(const ValueCompileProbe *probe, XrCompileResourceLimits limits) {
    size_t blocks = value_compile_live, bytes = value_compile_bytes, start = value_compile_calls;
    if (!probe->operation) {
        ValueCompileOwner owner = {0};
        CHECK(value_compile_owner_new(&owner, limits, probe->parameters) == XR_XIR_OK);
        XrXirTypeArena *occupied = (XrXirTypeArena *)(uintptr_t)1;
        XrXirTypeArena *before = occupied;
        XrCompileResourceStats stats = value_compile_stats(&owner.context);
        size_t physical_calls = value_compile_calls;
        CHECK(xr_xir_compile_type_arena_new(&owner.context, probe->types, &occupied) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!memcmp(&before, &occupied, sizeof(before)) && value_compile_calls == physical_calls);
        XrCompileResourceStats after = value_compile_stats(&owner.context);
        CHECK(!memcmp(&stats, &after, sizeof(stats)));
        value_compile_owner_release(&owner);
        CHECK(value_compile_live == blocks && value_compile_bytes == bytes);
        start = value_compile_calls;
    }
    XrCompileResourceStats exact = {0}; value_compile_injected = false;
    CHECK(value_compile_attempt(probe, limits, &exact) == XR_XIR_OK);
    CHECK(value_compile_live == blocks && value_compile_bytes == bytes);
    size_t sites = value_compile_calls - start; CHECK(sites > 1);
    for (size_t i = 0; i < sites; ++i) {
        value_compile_injected = false; value_compile_fail_at = value_compile_calls + i;
        CHECK(value_compile_attempt(probe, limits, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(value_compile_injected && value_compile_live == blocks && value_compile_bytes == bytes);
        value_compile_fail_at = SIZE_MAX;
    }
    XrCompileResourceLimits required = {exact.allocated_bytes, exact.peak_bytes, exact.work};
    CHECK(value_compile_attempt(probe, required, NULL) == XR_XIR_OK);
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrCompileResourceLimits cut = required;
        if (!mode) --cut.allocated_bytes; else if (mode == 1) --cut.live_bytes; else --cut.work;
        XrCompileResourceStats prefix = {0};
        CHECK(value_compile_attempt(probe, cut, &prefix) == XR_XIR_BUDGET);
        CHECK(prefix.allocated_bytes <= exact.allocated_bytes && prefix.work <= exact.work);
        CHECK(value_compile_live == blocks && value_compile_bytes == bytes);
    }
    required.work = 1;
    CHECK(value_compile_attempt(probe, required, NULL) == XR_XIR_BUDGET);
    CHECK(value_compile_live == blocks && value_compile_bytes == bytes);
    required.work = 0;
    CHECK(value_compile_attempt(probe, required, NULL) == XR_XIR_BUDGET);
    CHECK(value_compile_live == blocks && value_compile_bytes == bytes);
    fprintf(stderr, "value compiler probe op=%u: OOM=%zu allocated=%llu peak=%llu work=%llu physical=%zu/%zu\n",
        probe->operation, sites, (unsigned long long)exact.allocated_bytes,
        (unsigned long long)exact.peak_bytes, (unsigned long long)exact.work, blocks, bytes);
}
#endif // XIR_VALUE_COMPILE_OWNER_H
