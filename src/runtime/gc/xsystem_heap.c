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
#include "xregion.h"  // xr_region_free_raw_block (drain pooled blocks)
#include "../../base/xmalloc.h"
#include "../xshared.h"
#include "../../coro/xcoro_pool.h"
#include "../../coro/xcoroutine.h"
#include "xgc_header.h"         // XR_TCOROUTINE
#include "../object/xstring.h"  // STR_FLAG_GLOBAL
#include <string.h>
#include <stdio.h>
#include "../../os/os_mem.h"

// Large object threshold: use mmap for objects >= 64KB
#define XR_SHARED_MMAP_THRESHOLD (64 * 1024)

/* ========== Lifecycle API ========== */

static void sysheap_gc_pool_drain(XrSystemHeap *heap) {
    XR_DCHECK(heap != NULL, "sysheap_gc_pool_drain: NULL heap");
    xr_mutex_lock(&heap->gc_pool_mu);
    struct XrCoroGC *gc = heap->gc_pool_head;
    while (gc) {
        struct XrCoroGC *next = *(struct XrCoroGC **) gc;
        xr_free(gc);
        gc = next;
    }
    heap->gc_pool_head = NULL;
    heap->gc_pool_count = 0;
    xr_mutex_unlock(&heap->gc_pool_mu);
}

static void sysheap_block_pool_drain(XrSystemHeap *heap) {
    XR_DCHECK(heap != NULL, "sysheap_block_pool_drain: NULL heap");
    xr_mutex_lock(&heap->block_pool_mu);
    void *b = heap->block_pool_head;
    while (b) {
        void *next = *(void **) b; /* linked via XrRegionBlock.next (first word) */
        xr_region_free_raw_block(b);
        b = next;
    }
    heap->block_pool_head = NULL;
    heap->block_pool_count = 0;
    xr_mutex_unlock(&heap->block_pool_mu);
}

void *xr_sysheap_block_pool_pop(XrSystemHeap *heap) {
    if (!heap || !heap->initialized)
        return NULL;
    xr_mutex_lock(&heap->block_pool_mu);
    void *b = heap->block_pool_head;
    if (b) {
        heap->block_pool_head = *(void **) b;
        heap->block_pool_count--;
    }
    xr_mutex_unlock(&heap->block_pool_mu);
    return b;
}

bool xr_sysheap_block_pool_push(XrSystemHeap *heap, void *block) {
    if (!heap || !heap->initialized || !block)
        return false;
    xr_mutex_lock(&heap->block_pool_mu);
    if (heap->block_pool_count >= XR_SYSHEAP_BLOCK_POOL_MAX) {
        xr_mutex_unlock(&heap->block_pool_mu);
        return false;
    }
    *(void **) block = heap->block_pool_head;
    heap->block_pool_head = block;
    heap->block_pool_count++;
    xr_mutex_unlock(&heap->block_pool_mu);
    return true;
}

bool xr_sysheap_init(XrSystemHeap *heap, const XrSysHeapConfig *config) {
    /* Public init API: caller-side error rather than assertion so a NULL
     * heap turns into a clean failure instead of an abort in release. */
    if (!heap)
        return false;

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

    // Initialize XrCoroGC L2 pool
    xr_mutex_init(&heap->gc_pool_mu);
    heap->gc_pool_head = NULL;
    heap->gc_pool_count = 0;

    // Initialize Region block L2 pool
    xr_mutex_init(&heap->block_pool_mu);
    heap->block_pool_head = NULL;
    heap->block_pool_count = 0;

    heap->initialized = true;
    return true;
}

void xr_sysheap_destroy_coro_storage(XrSystemHeap *heap) {
    if (!heap || !heap->initialized)
        return;

    if (heap->coro_pool) {
        xr_coro_pool_destroy(heap->coro_pool);
        xr_free(heap->coro_pool);
        heap->coro_pool = NULL;
    }

    // Drain XrCoroGC L2 pool after coroutine pool teardown. Coroutine
    // finalizers may recycle GC structs back into this pool while their
    // shells are being released.
    sysheap_gc_pool_drain(heap);

    // Drain the Region block L2 pool: coroutine heap teardown and worker exit
    // push recycled blocks here; return them to the OS now.
    sysheap_block_pool_drain(heap);
}

void xr_sysheap_destroy(XrSystemHeap *heap) {
    if (!heap || !heap->initialized)
        return;

    xr_sysheap_destroy_coro_storage(heap);
    xr_mutex_destroy(&heap->gc_pool_mu);
    xr_mutex_destroy(&heap->block_pool_mu);

    // Destroy class arena
    xr_arena_destroy(&heap->class_arena);

    heap->initialized = false;
}

/* ========== XrCoroGC Struct Pool (L2) ========== */

struct XrCoroGC *xr_sysheap_gc_pool_pop(XrSystemHeap *heap) {
    if (!heap || !heap->initialized)
        return NULL;
    xr_mutex_lock(&heap->gc_pool_mu);
    struct XrCoroGC *gc = heap->gc_pool_head;
    if (gc) {
        heap->gc_pool_head = *(struct XrCoroGC **) gc;
        heap->gc_pool_count--;
    }
    xr_mutex_unlock(&heap->gc_pool_mu);
    return gc;
}

bool xr_sysheap_gc_pool_push(XrSystemHeap *heap, struct XrCoroGC *gc) {
    if (!heap || !heap->initialized || !gc)
        return false;
    xr_mutex_lock(&heap->gc_pool_mu);
    if (heap->gc_pool_count >= XR_SYSHEAP_GC_POOL_MAX) {
        xr_mutex_unlock(&heap->gc_pool_mu);
        return false;
    }
    *(struct XrCoroGC **) gc = heap->gc_pool_head;
    heap->gc_pool_head = gc;
    heap->gc_pool_count++;
    xr_mutex_unlock(&heap->gc_pool_mu);
    return true;
}

/* ========== Coroutine Allocation API ========== */

struct XrCoroutine *xr_sysheap_alloc_coro(XrSystemHeap *heap) {
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

void *xr_sysheap_alloc_shared(XrSystemHeap *heap, size_t size, uint8_t type) {
    XR_DCHECK(size > 0, "sysheap_alloc_shared: zero size");
    if (!heap || !heap->initialized)
        return NULL;

    XrGCHeader *obj = NULL;
    bool use_mmap = (size >= XR_SHARED_MMAP_THRESHOLD);

    if (use_mmap) {
        // Large objects use anonymous mmap (xr_mem_map) to avoid
        // heap fragmentation.
        obj = (XrGCHeader *) xr_mem_map(size, XR_MEM_PROT_READ | XR_MEM_PROT_WRITE);
        if (!obj) {
            return NULL;
        }
        // Anonymous pages are guaranteed zero-initialised.
        obj->type = type;
        obj->objsize = (uint32_t) size;
        // Mark as shared and mmap-allocated (in extra bit 13)
        obj->extra = XR_GC_STORAGE_SHARED | XR_GC_FLAG_MMAP;
        obj->_rsv = 0;
        atomic_fetch_add(&heap->stats.shared_mmap_count, 1);
    } else {
        // Small objects use regular malloc
        obj = (XrGCHeader *) xr_malloc(size);
        if (obj) {
            memset(obj, 0, size);
            obj->type = type;
            obj->extra = XR_GC_STORAGE_SHARED;
        }
    }

    if (obj) {
        /* Establish the shared-object invariant in the allocator itself:
         * atomic RC = 1 (XR_OBJ_ATOMIC set, refcount stored as -1). Callers
         * may still call xr_shared_set_refc with their own count; doing so is
         * idempotent. Leaving refcount at 0 here (thread-local "unique"
         * encoding) was a latent hazard: a drop before the caller's set_refc
         * would free a live shared object. */
        xr_shared_set_refc(obj, 1);
        atomic_fetch_add(&heap->stats.shared_alloc_count, 1);
    }
    return obj;
}

// Free shared object (handles both malloc and mmap)
void xr_sysheap_free_shared(void *ptr, size_t size) {
    if (!ptr)
        return;

    XrGCHeader *obj = (XrGCHeader *) ptr;
    if (XR_GC_IS_MMAP(obj)) {
        xr_mem_unmap(ptr, size);
    } else {
        xr_free(ptr);
    }
}

/* ========== Class/Module Allocation API ========== */

void *xr_sysheap_alloc_class(XrSystemHeap *heap, size_t size) {
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

void *xr_sysheap_alloc_module(XrSystemHeap *heap, size_t size) {
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
    if (!heap || !stats)
        return;

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
    printf("  Alloc: %llu, Free: %llu, Reuse: %llu\n", (unsigned long long) stats.coro_alloc_count,
           (unsigned long long) stats.coro_free_count, (unsigned long long) stats.coro_reuse_count);
    printf("Class Arena:\n");
    printf("  Classes: %llu, Bytes: %llu\n", (unsigned long long) stats.class_alloc_count,
           (unsigned long long) stats.class_alloc_bytes);
    printf("  Modules: %llu\n", (unsigned long long) stats.module_alloc_count);
    printf("Shared:\n");
    printf("  Shared: %llu\n", (unsigned long long) stats.shared_alloc_count);
    printf("  Arena total: %zu bytes\n", heap->class_arena.total_allocated);
    printf("==============================\n");
}

/* ========== Shared Object Destruction ========== */

// g_type_ops declared in xgc_internal.h as extern const

void xr_shared_destroy(XrGCHeader *obj) {
    if (!obj)
        return;

    /* Global pool strings are owned by XrGlobalStringPool, not by coroutine GC.
     * They are freed in xr_global_pool_free during isolate shutdown. */
    if (XR_GC_GET_TYPE(obj) == XR_TSTRING && (obj->extra & STR_FLAG_GLOBAL)) {
        return;
    }

    uint8_t type = XR_GC_GET_TYPE(obj);

    // Call destructor if registered (to free internal resources like buffers)
    if (type < XGC_MAX_TYPES && g_type_ops[type].destroy) {
        g_type_ops[type].destroy(obj, NULL);
    }

    // Free the object itself
    if (XR_GC_IS_MMAP(obj)) {
        xr_mem_unmap(obj, obj->objsize);
    } else {
        xr_free(obj);
    }
}
