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
#include "./xmalloc.h"

// Thread-local segment cache for multi-Isolate support (no lock needed)

#define XR_ARENA_MAX_CACHED_SEGMENTS 8

static __thread XrArenaSegment *tls_segment_cache = NULL;
static __thread int tls_segment_cache_count = 0;

// Align to 8-byte boundary
static inline size_t align_size(size_t size) {
    return (size + XR_ARENA_ALIGNMENT - 1) & ~(XR_ARENA_ALIGNMENT - 1);
}

static XrArenaSegment *cache_get_segment(size_t capacity) {
    if (capacity != XR_ARENA_SEGMENT_SIZE) {
        return NULL;
    }
    
    XrArenaSegment *seg = tls_segment_cache;
    if (seg) {
        tls_segment_cache = seg->next;
        tls_segment_cache_count--;
        seg->next = NULL;
        seg->size = 0;
    }
    return seg;
}

static bool cache_put_segment(XrArenaSegment *seg) {
    if (!seg || seg->capacity != XR_ARENA_SEGMENT_SIZE) {
        return false;
    }
    
    if (tls_segment_cache_count >= XR_ARENA_MAX_CACHED_SEGMENTS) {
        return false;
    }
    seg->next = tls_segment_cache;
    tls_segment_cache = seg;
    tls_segment_cache_count++;
    XR_DCHECK(tls_segment_cache_count <= XR_ARENA_MAX_CACHED_SEGMENTS, "arena cache: count > max");
    return true;
}

static XrArenaSegment *allocate_segment(size_t capacity) {
    // Try cache first
    XrArenaSegment *seg = cache_get_segment(capacity);
    if (seg) {
        return seg;
    }
    
    seg = (XrArenaSegment*)xr_malloc(sizeof(XrArenaSegment) + capacity);
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
    if (!arena) return;
    
    if (initial_size == 0) {
        initial_size = XR_ARENA_SEGMENT_SIZE;
    }
    
    initial_size = align_size(initial_size);
    
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
    if (!arena || size == 0) return NULL;
    
    // Align to 8 bytes
    size = align_size(size);
    
    // Need new segment?
    if (arena->position + size > arena->limit) {
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
    
    void *result = arena->position;
    arena->position += size;
    arena->total_allocated += size;
    XR_DCHECK(arena->position <= arena->limit, "arena_alloc: position > limit");
    
    arena->head->size += size;
    return result;
}

void *xr_arena_alloc(XrArena *arena, size_t size) {
    void *result = bump_alloc(arena, size);
    if (result) memset(result, 0, size);
    return result;
}

void *xr_arena_alloc_raw(XrArena *arena, size_t size) {
    return bump_alloc(arena, size);
}

void xr_arena_destroy(XrArena *arena) {
    if (!arena) return;
    
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
    if (!arena || !arena->head) return;
    
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
    if (!arena || !str) return NULL;
    
    size_t len = strlen(str) + 1;
    char *copy = (char*)xr_arena_alloc(arena, len);
    
    if (copy) {
        memcpy(copy, str, len);
    }
    
    return copy;
}

char *xr_arena_strndup(XrArena *arena, const char *str, size_t len) {
    if (!arena || !str) return NULL;
    
    char *copy = (char*)xr_arena_alloc(arena, len + 1);
    
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    
    return copy;
}

size_t xr_arena_get_allocated_size(XrArena *arena) {
    return arena ? arena->total_allocated : 0;
}

void xr_arena_get_stats(XrArena *arena, XrArenaStats *stats) {
    if (!stats) return;
    
    stats->segment_count = 0;
    stats->total_capacity = 0;
    stats->total_used = 0;
    
    if (!arena) return;
    
    XrArenaSegment *seg = arena->head;
    while (seg) {
        stats->segment_count++;
        stats->total_capacity += seg->capacity;
        stats->total_used += seg->size;
        seg = seg->next;
    }
}
