/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_callable52_graph_oom_probe.c - Frozen callable graph allocation failure controls
 */
#include "base/xmalloc.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "ir/xi.h"
#include <stdio.h>
#include <stdlib.h>

static size_t fail_at = SIZE_MAX;
static size_t attempts;
static size_t live;

static void *probe_malloc(size_t size) {
    if (attempts++ == fail_at)
        return NULL;
    void *pointer = xr_malloc(size);
    if (pointer)
        live++;
    return pointer;
}

static void *probe_calloc(size_t count, size_t size) {
    if (attempts++ == fail_at)
        return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer)
        live++;
    return pointer;
}

static void probe_free(void *pointer) {
    if (pointer) {
        if (live == 0)
            abort();
        live--;
    }
    xr_free(pointer);
}

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(size) probe_malloc(size)
#define xr_calloc(count, size) probe_calloc(count, size)
#define xr_free(pointer) probe_free(pointer)
#include "plan/semantic/xr_semantic_coroutine_module_shape.h"

static void require(bool condition) {
    if (!condition) {
        fprintf(stderr, "callable graph allocator control failed attempts=%zu live=%zu\n", attempts, live);
        abort();
    }
}

XR_FUNC void probe_callable52_graph_oom(const XrSemanticCoroutineModuleGraph *graph,
                                       uint32_t owner, uint32_t function, int expected) {
    attempts = live = 0;
    fail_at = SIZE_MAX;
    require(xr_semantic_function_graph_suspendability(graph, owner, function) == expected);
    size_t total = attempts;
    require(total > 0 && live == 0);
    for (size_t failure = 0; failure < total; failure++) {
        attempts = live = 0;
        fail_at = failure;
        require(xr_semantic_function_graph_suspendability(graph, owner, function) == -1);
        require(live == 0);
    }
    fail_at = SIZE_MAX;
    printf("callable graph expected=%d allocation_failures=%zu live=0\n", expected, total);
}
