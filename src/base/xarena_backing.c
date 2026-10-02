/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarena_backing.c - System storage adapter without compiler dependencies
 */
#include "xarena_backing.h"
#include "xmalloc.h"

static XrArenaStatus arena_system_alloc(void *context, size_t bytes, void **output) {
    (void) context;
    if (!bytes || !output || *output) return XR_ARENA_BAD_ARGUMENT;
    void *memory = xr_malloc(bytes);
    if (!memory) return XR_ARENA_OUT_OF_MEMORY;
    *output = memory;
    return XR_ARENA_OK;
}

static void arena_system_free(void *context, void *memory) {
    (void) context;
    xr_free(memory);
}

static XrArenaStatus arena_system_work(void *context, uint64_t units) {
    (void) context;
    (void) units;
    return XR_ARENA_OK;
}

static XrArenaStatus arena_system_retain(void *context) {
    (void) context;
    return XR_ARENA_OK;
}

static void arena_system_release(void *context) {
    (void) context;
}

XR_FUNC XrArenaBacking xr_arena_system_backing(void) {
    return (XrArenaBacking) {NULL, arena_system_alloc, arena_system_free,
        arena_system_work, arena_system_retain, arena_system_release};
}
