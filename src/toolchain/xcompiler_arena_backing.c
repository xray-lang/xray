/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_arena_backing.c - Shared ledger adapter for compiler arena storage
 */
#include "xcompiler_arena_backing.h"
#include "base/xcompile_resources.h"
#include "base/xcompile_state.h"
#include "base/xarena.h"

static XrArenaStatus compiler_arena_status(XrCompileResourceStatus status) {
    switch (status) {
        case XR_COMPILE_RESOURCE_OK: return XR_ARENA_OK;
        case XR_COMPILE_RESOURCE_BAD_ARGUMENT: return XR_ARENA_BAD_ARGUMENT;
        case XR_COMPILE_RESOURCE_BUDGET: return XR_ARENA_BUDGET;
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_ARENA_OUT_OF_MEMORY;
    }
    return XR_ARENA_BAD_ARGUMENT;
}

static XrArenaStatus compiler_arena_alloc(void *context, size_t bytes, void **output) {
    return compiler_arena_status(xr_compile_resources_alloc(context, bytes, output));
}

static void compiler_arena_free(void *context, void *memory) {
    (void) context;
    xr_compile_resources_free(memory);
}

static XrArenaStatus compiler_arena_work(void *context, uint64_t units) {
    return compiler_arena_status(xr_compile_resources_work(context, units));
}

static XrArenaStatus compiler_arena_retain(void *context) {
    return compiler_arena_status(xr_compile_resources_retain(context));
}

static void compiler_arena_release(void *context) {
    xr_compile_resources_release(context);
}

XR_FUNC XrArenaStatus xr_compiler_arena_backing(
    XrCompileResources *resources, XrArenaBacking *output) {
    if (!resources || !output) return XR_ARENA_BAD_ARGUMENT;
    *output = (XrArenaBacking) {resources, compiler_arena_alloc, compiler_arena_free,
        compiler_arena_work, compiler_arena_retain, compiler_arena_release};
    return XR_ARENA_OK;
}

static XrArenaStatus compiler_state_arena_alloc(void *context, size_t bytes, void **output) {
    return compiler_arena_status(xr_compile_state_alloc(context, bytes, output));
}

static XrArenaStatus compiler_state_arena_work(void *context, uint64_t units) {
    return compiler_arena_status(xr_compile_state_work(context, units));
}

static XrArenaStatus compiler_state_arena_retain(void *context) {
    return compiler_arena_status(xr_compile_state_retain(context));
}

static void compiler_state_arena_release(void *context) {
    xr_compile_state_release(context);
}

XR_FUNC XrArenaStatus xr_compiler_arena_state_backing(XrCompileState *state, XrArenaBacking *output) {
    if (!state || !output) return XR_ARENA_BAD_ARGUMENT;
    *output = (XrArenaBacking) {state, compiler_state_arena_alloc, compiler_arena_free,
        compiler_state_arena_work, compiler_state_arena_retain, compiler_state_arena_release};
    return XR_ARENA_OK;
}

XR_FUNC bool xr_compiler_arena_matches_state(const XrArena *arena, const XrCompileState *state) {
    return arena && state && arena->retained && arena->backing.context == state &&
        arena->backing.alloc == compiler_state_arena_alloc && arena->backing.free == compiler_arena_free &&
        arena->backing.work == compiler_state_arena_work && arena->backing.retain == compiler_state_arena_retain &&
        arena->backing.release == compiler_state_arena_release;
}

XR_FUNC XrCompileResourceStatus xr_compiler_arena_capture_status(const XrArena *arena, XrCompileState *state) {
    if (!xr_compiler_arena_matches_state(arena, state))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    switch (xr_arena_status(arena)) {
        case XR_ARENA_OK: return xr_compile_state_status(state);
        case XR_ARENA_BAD_ARGUMENT: return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        case XR_ARENA_BUDGET: return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
        case XR_ARENA_OUT_OF_MEMORY: return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}
