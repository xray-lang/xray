/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsystem_heap.c - System heap manager implementation
 *
 * KEY CONCEPT:
 *   Integrates existing components:
 *   - XrCoroStructPool (coroutine object pool)
 *   - XrArena (class/module arena)
 */

#include "xsystem_heap.h"
#include "../../base/xchecks.h"
#include "xgc_internal.h"
#include "../../base/xmalloc.h"
#include "../xshared.h"
#include "../../coro/xcoro_pool.h"
#include "../../coro/xcoroutine.h"
#include "xgc_header.h" // XR_TCOROUTINE
#include "../object/xstring.h" // STR_FLAG_GLOBAL
#include <string.h>
#include <stdio.h>
#include <sys/mman.h> // mmap for large objects

// Large object threshold: use mmap for objects >= 64KB
#define XR_SHARED_MMAP_THRESHOLD (64 * 1024)

/* ========== Lifecycle API ========== */

bool xr_sysheap_init(XrSystemHeap *heap, const XrSysHeapConfig *config) {
    XR_DCHECK(heap != NULL, "sysheap_init: NULL heap");
    if (!heap) return false;

    memset(heap, 0, sizeof(XrSystemHeap));

    // Use default config
    size_t coro_pool_size = XR_SYSHEAP_DEFAULT_CORO_POOL_SIZE;
    size_t class_arena_size = XR_SYSHEAP_DEFAULT_CLASS_ARENA_SIZE;

    if (config) {
        if (config->coro_pool_init_size > 0) {
            coro_pool_size = config->coro_pool_init_size;
        }
        if (config->class_arena_init_size > 0) {
            class_arena_size = config->class_arena_init_size;
        }
    }

    // Create coroutine pool
    heap->coro_pool = xr_malloc(sizeof(XrCoroStructPool));
    if (!heap->coro_pool) {
        return false;
    }

    if (!xr_coro_pool_init(heap->coro_pool, coro_pool_size)) {
        xr_free(heap->coro_pool);
        heap->coro_pool = NULL;
        return false;
    }

    // Initialize class arena
    xr_arena_init(&heap->class_arena, class_arena_size);

    heap->initialized = true;
    return true;
}

void xr_sysheap_destroy(XrSystemHeap *heap) {
    if (!heap || !heap->initialized) return;

    // Destroy coroutine pool
    if (heap->coro_pool) {
        xr_coro_pool_destroy(heap->coro_pool);
        xr_free(heap->coro_pool);
        heap->coro_pool = NULL;
    }

    // Destroy class arena
    xr_arena_destroy(&heap->class_arena);

    heap->initialized = false;
}

/* ========== Coroutine Allocation API ========== */

struct XrCoroutine* xr_sysheap_alloc_coro(XrSystemHeap *heap) {
    if (!heap || !heap->initialized || !heap->coro_pool) {
        return NULL;
    }

    struct XrCoroutine *coro = xr_coro_pool_alloc(heap->coro_pool);
    if (coro) {
        // Set GC type (coroutine objects need correct type identifier)
        coro->gc.type = XR_TCOROUTINE;
        heap->stats.coro_alloc_count++;
    }

    return coro;
}

void xr_sysheap_free_coro(XrSystemHeap *heap, struct XrCoroutine *coro) {
    if (!heap || !heap->initialized || !heap->coro_pool || !coro) {
        return;
    }

    xr_coro_struct_pool_free(heap->coro_pool, coro);
    heap->stats.coro_free_count++;
}

/* ========== Shared Object Allocation API ========== */

void* xr_sysheap_alloc_shared(XrSystemHeap *heap, size_t size, uint8_t type) {
    XR_DCHECK(size > 0, "sysheap_alloc_shared: zero size");
    if (!heap || !heap->initialized) return NULL;

    XrGCHeader *obj = NULL;
    bool use_mmap = (size >= XR_SHARED_MMAP_THRESHOLD);

    if (use_mmap) {
        // Large objects use mmap to avoid heap fragmentation
        obj = (XrGCHeader*)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (obj == MAP_FAILED) {
            return NULL;
        }
        // MAP_ANONYMOUS guarantees zero-initialized memory
        obj->type = type;
        obj->objsize = (uint32_t)size;
        // Mark as shared and mmap-allocated (in extra bit 13)
        obj->extra = XR_GC_STORAGE_SHARED | XR_GC_FLAG_MMAP;
        obj->marked = 0;
        atomic_fetch_add(&heap->stats.shared_mmap_count, 1);
    } else {
        // Small objects use regular malloc
        obj = (XrGCHeader*)xr_malloc(size);
        if (obj) {
            memset(obj, 0, size);
            obj->type = type;
            obj->extra = XR_GC_STORAGE_SHARED;
        }
    }

    if (obj) {
        atomic_fetch_add(&heap->stats.shared_alloc_count, 1);
    }
    return obj;
}

// Free shared object (handles both malloc and mmap)
void xr_sysheap_free_shared(void *ptr, size_t size) {
    if (!ptr) return;

    XrGCHeader *obj = (XrGCHeader*)ptr;
    if (XR_GC_IS_MMAP(obj)) {
        munmap(ptr, size);
    } else {
        xr_free(ptr);
    }
}

/* ========== Class/Module Allocation API ========== */

void* xr_sysheap_alloc_class(XrSystemHeap *heap, size_t size) {
    XR_DCHECK(size > 0, "sysheap_alloc_class: zero size");
    if (!heap || !heap->initialized) {
        return NULL;
    }

    void *ptr = xr_arena_alloc(&heap->class_arena, size);
    if (ptr) {
        atomic_fetch_add(&heap->stats.class_alloc_count, 1);
        atomic_fetch_add(&heap->stats.class_alloc_bytes, size);
    }

    return ptr;
}

void* xr_sysheap_alloc_module(XrSystemHeap *heap, size_t size) {
    XR_DCHECK(size > 0, "sysheap_alloc_module: zero size");
    if (!heap || !heap->initialized) {
        return NULL;
    }

    void *ptr = xr_arena_alloc(&heap->class_arena, size);
    if (ptr) {
        atomic_fetch_add(&heap->stats.module_alloc_count, 1);
    }

    return ptr;
}

/* ========== Statistics API ========== */

void xr_sysheap_get_stats(XrSystemHeap *heap, XrSysHeapStats *stats) {
    if (!heap || !stats) return;

    stats->coro_alloc_count = heap->stats.coro_alloc_count;
    stats->coro_free_count = heap->stats.coro_free_count;
    stats->coro_reuse_count = heap->stats.coro_reuse_count;
    stats->class_alloc_count = atomic_load(&heap->stats.class_alloc_count);
    stats->class_alloc_bytes = atomic_load(&heap->stats.class_alloc_bytes);
    stats->module_alloc_count = atomic_load(&heap->stats.module_alloc_count);
    stats->shared_alloc_count = atomic_load(&heap->stats.shared_alloc_count);
}

void xr_sysheap_print_stats(XrSystemHeap *heap) {
    if (!heap || !heap->initialized) {
        printf("[SystemHeap] Not initialized\n");
        return;
    }

    XrSysHeapStats stats;
    xr_sysheap_get_stats(heap, &stats);

    printf("=== Xray System Heap Stats ===\n");
    printf("Allocator: %s\n", xr_mem_get_allocator_name());
    printf("Coroutine Pool:\n");
    printf("  Alloc: %llu, Free: %llu, Reuse: %llu\n",
           (unsigned long long)stats.coro_alloc_count,
           (unsigned long long)stats.coro_free_count,
           (unsigned long long)stats.coro_reuse_count);
    printf("Class Arena:\n");
    printf("  Classes: %llu, Bytes: %llu\n",
           (unsigned long long)stats.class_alloc_count,
           (unsigned long long)stats.class_alloc_bytes);
    printf("  Modules: %llu\n",
           (unsigned long long)stats.module_alloc_count);
    printf("Shared:\n");
    printf("  Shared: %llu\n",
           (unsigned long long)stats.shared_alloc_count);
    printf("  Arena total: %zu bytes\n",
           heap->class_arena.total_allocated);
    printf("==============================\n");
}

/* ========== Shared Object Destruction ========== */

// g_destroy_funcs declared in xgc_internal.h as extern const

void xr_shared_destroy(XrGCHeader *obj) {
    if (!obj) return;

    /* Global pool strings are owned by XrGlobalStringPool, not by coroutine GC.
     * They are freed in xr_global_pool_free during isolate shutdown. */
    if (XR_GC_GET_TYPE(obj) == XR_TSTRING && (obj->extra & STR_FLAG_GLOBAL)) {
        return;
    }

    uint8_t type = XR_GC_GET_TYPE(obj);

    // Call destructor if registered (to free internal resources like buffers)
    if (type < XGC_MAX_TYPES && g_destroy_funcs[type]) {
        g_destroy_funcs[type](obj, NULL);
    }

    // Free the object itself
    if (XR_GC_IS_MMAP(obj)) {
        munmap(obj, obj->objsize);
    } else {
        xr_free(obj);
    }
}
