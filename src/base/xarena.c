/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarena.c - Arena memory allocator implementation
 */

#include "xarena.h"
#include "xchecks.h"
#include "xlog.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "./xmalloc.h"

// Thread-local segment cache for multi-Isolate support (no lock needed).
// Exempt from "no mutable file-scope globals" rule:
// XR_THREAD_LOCAL gives per-thread isolation with no shared mutable state.

#define XR_ARENA_MAX_CACHED_SEGMENTS 8

typedef struct {
    XrArenaSegment *segments;
    int count;
} XrArenaSegmentCache;

static XR_THREAD_LOCAL XrArenaSegmentCache tls_cache = {NULL, 0};

// Align to 8-byte boundary
static inline bool align_size(size_t size, size_t *aligned_size) {
    if (!aligned_size)
        return false;
    if (size > SIZE_MAX - (XR_ARENA_ALIGNMENT - 1))
        return false;
    *aligned_size = (size + XR_ARENA_ALIGNMENT - 1) & ~((size_t) XR_ARENA_ALIGNMENT - 1);
    return true;
}

static XrArenaSegment *cache_get_segment(size_t capacity) {
    if (capacity != XR_ARENA_SEGMENT_SIZE) {
        return NULL;
    }

    XrArenaSegment *seg = tls_cache.segments;
    if (seg) {
        tls_cache.segments = seg->next;
        tls_cache.count--;
        seg->next = NULL;
        seg->size = 0;
    }
    return seg;
}

static bool cache_put_segment(XrArenaSegment *seg) {
    if (!seg || seg->capacity != XR_ARENA_SEGMENT_SIZE) {
        return false;
    }

    if (tls_cache.count >= XR_ARENA_MAX_CACHED_SEGMENTS) {
        return false;
    }
    seg->next = tls_cache.segments;
    tls_cache.segments = seg;
    tls_cache.count++;
    XR_DCHECK(tls_cache.count <= XR_ARENA_MAX_CACHED_SEGMENTS, "arena cache: count > max");
    return true;
}

static XrArenaSegment *allocate_segment(size_t capacity) {
    if (capacity > SIZE_MAX - sizeof(XrArenaSegment)) {
        return NULL;
    }

    // Try cache first
    XrArenaSegment *seg = cache_get_segment(capacity);
    if (seg) {
        return seg;
    }

    seg = (XrArenaSegment *) xr_malloc(sizeof(XrArenaSegment) + capacity);
    if (!seg) {
        xr_log_warning("arena", "failed to allocate segment of size %zu", capacity);
        return NULL;
    }

    seg->next = NULL;
    seg->size = 0;
    seg->capacity = capacity;

    return seg;
}

void xr_arena_init(XrArena *arena, size_t initial_size) {
    if (!arena)
        return;

    if (initial_size == 0) {
        initial_size = XR_ARENA_SEGMENT_SIZE;
    }

    if (!align_size(initial_size, &initial_size)) {
        arena->head = NULL;
        arena->position = NULL;
        arena->limit = NULL;
        arena->total_allocated = 0;
        return;
    }

    XrArenaSegment *seg = allocate_segment(initial_size);
    if (!seg) {
        arena->head = NULL;
        arena->position = NULL;
        arena->limit = NULL;
        arena->total_allocated = 0;
        return;
    }

    arena->head = seg;
    arena->position = seg->data;
    arena->limit = seg->data + seg->capacity;
    arena->total_allocated = 0;
}

// Internal bump allocator (no zeroing)
static void *bump_alloc(XrArena *arena, size_t size) {
    if (!arena || size == 0)
        return NULL;

    // Align to 8 bytes
    if (!align_size(size, &size))
        return NULL;
    if (!arena->position || !arena->limit || !arena->head)
        return NULL;

    // Need new segment?
    size_t available = (size_t) (arena->limit - arena->position);
    if (size > available) {
        size_t new_capacity = size > XR_ARENA_SEGMENT_SIZE ? size : XR_ARENA_SEGMENT_SIZE;

        XrArenaSegment *new_seg = allocate_segment(new_capacity);
        if (!new_seg) {
            return NULL;
        }

        new_seg->next = arena->head;
        arena->head = new_seg;

        arena->position = new_seg->data;
        arena->limit = new_seg->data + new_seg->capacity;
    }

    if (arena->total_allocated > SIZE_MAX - size)
        return NULL;
    if (arena->head->size > SIZE_MAX - size)
        return NULL;

    void *result = arena->position;
    arena->position += size;
    arena->total_allocated += size;
    XR_DCHECK(arena->position <= arena->limit, "arena_alloc: position > limit");

    arena->head->size += size;
    return result;
}

void *xr_arena_alloc(XrArena *arena, size_t size) {
    void *result = bump_alloc(arena, size);
    if (result)
        memset(result, 0, size);
    return result;
}

XR_FUNCDEF void *xr_arena_alloc_array(XrArena *arena, size_t elem_size, size_t count) {
    if (elem_size == 0 || count == 0)
        return NULL;
    if (count > SIZE_MAX / elem_size)
        return NULL;
    return xr_arena_alloc(arena, elem_size * count);
}

void *xr_arena_alloc_raw(XrArena *arena, size_t size) {
    return bump_alloc(arena, size);
}

void xr_arena_destroy(XrArena *arena) {
    if (!arena)
        return;

    XrArenaSegment *seg = arena->head;
    while (seg) {
        XrArenaSegment *next = seg->next;
        if (!cache_put_segment(seg)) {
            xr_free(seg);
        }
        seg = next;
    }

    arena->head = NULL;
    arena->position = NULL;
    arena->limit = NULL;
    arena->total_allocated = 0;
}

void xr_arena_reset(XrArena *arena) {
    if (!arena || !arena->head)
        return;

    // Release non-head segments back to cache or free
    XrArenaSegment *seg = arena->head->next;
    while (seg) {
        XrArenaSegment *next = seg->next;
        if (!cache_put_segment(seg)) {
            xr_free(seg);
        }
        seg = next;
    }

    // Reset head segment only
    arena->head->next = NULL;
    arena->head->size = 0;
    arena->position = arena->head->data;
    arena->limit = arena->head->data + arena->head->capacity;
    arena->total_allocated = 0;
}

char *xr_arena_strdup(XrArena *arena, const char *str) {
    if (!arena || !str)
        return NULL;

    size_t len = strlen(str) + 1;
    char *copy = (char *) xr_arena_alloc(arena, len);

    if (copy) {
        memcpy(copy, str, len);
    }

    return copy;
}

char *xr_arena_strndup(XrArena *arena, const char *str, size_t len) {
    if (!arena || !str)
        return NULL;
    if (len == SIZE_MAX)
        return NULL;

    char *copy = (char *) xr_arena_alloc(arena, len + 1);

    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }

    return copy;
}

size_t xr_arena_get_allocated_size(XrArena *arena) {
    return arena ? arena->total_allocated : 0;
}

XrArenaState xr_arena_save(XrArena *arena) {
    XR_DCHECK(arena != NULL, "arena_save: NULL arena");
    XrArenaState state = {
        .head = arena->head,
        .position = arena->position,
        .total_allocated = arena->total_allocated,
    };
    return state;
}

void xr_arena_restore(XrArena *arena, XrArenaState state) {
    XR_DCHECK(arena != NULL, "arena_restore: NULL arena");
    // Only valid if no new segments were allocated since save.
    // If head changed, allocations spanned a new segment boundary
    // and we cannot safely rewind (old segment data is abandoned).
    XR_DCHECK(arena->head == state.head,
              "arena_restore: head segment changed since save, cannot rewind across segments");
    arena->position = state.position;
    arena->total_allocated = state.total_allocated;
    arena->head->size = (size_t) (state.position - arena->head->data);
}

void xr_arena_get_stats(XrArena *arena, XrArenaStats *stats) {
    if (!stats)
        return;

    stats->segment_count = 0;
    stats->total_capacity = 0;
    stats->total_used = 0;

    if (!arena)
        return;

    XrArenaSegment *seg = arena->head;
    while (seg) {
        stats->segment_count++;
        stats->total_capacity += seg->capacity;
        stats->total_used += seg->size;
        seg = seg->next;
    }
}
