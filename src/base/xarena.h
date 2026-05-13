/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarena.h - Arena memory allocator
 *
 * KEY CONCEPT:
 *   Arena = fast bump allocator with bulk free. No per-object free.
 *   Ideal for short-lived, phase-based allocations (parse, compile).
 *
 * WHY THIS DESIGN:
 *   - O(1) allocation (just pointer bump)
 *   - Zero fragmentation within a phase
 *   - Single free() releases entire phase's memory
 *
 * USE CASES:
 *   - AST nodes during parsing (freed after compile)
 *   - Compiler temporaries (freed after code generation)
 *   - Class/Module definitions (XrSystemHeap uses this)
 *
 * RELATED MODULES:
 *   - xalloc.h: General malloc/free (when individual free needed)
 *   - xsystem_heap.h: Uses Arena for class/module storage
 */

#ifndef XARENA_H
#define XARENA_H

#include <stddef.h>
#include <stdint.h>
#include "xdefs.h"

#define XR_ARENA_SEGMENT_SIZE (64 * 1024)
#define XR_ARENA_ALIGNMENT 8

typedef struct XrArenaSegment {
    struct XrArenaSegment *next;
    size_t size;
    size_t capacity;
    char data[];
} XrArenaSegment;

typedef struct XrArena {
    XrArenaSegment *head;
    char *position;
    char *limit;
    size_t total_allocated;
} XrArena;

XR_FUNC void xr_arena_init(XrArena *arena, size_t initial_size);
XR_FUNC void *xr_arena_alloc(XrArena *arena, size_t size);
XR_FUNC void *xr_arena_alloc_array(XrArena *arena, size_t elem_size, size_t count);
XR_FUNC void *xr_arena_alloc_raw(XrArena *arena, size_t size);  // no zeroing
XR_FUNC void xr_arena_destroy(XrArena *arena);
XR_FUNC void xr_arena_reset(XrArena *arena);
XR_FUNC char *xr_arena_strdup(XrArena *arena, const char *str);
XR_FUNC char *xr_arena_strndup(XrArena *arena, const char *str, size_t len);
XR_FUNC size_t xr_arena_get_allocated_size(XrArena *arena);

// Savepoint: mark/rewind for tentative allocations (e.g. speculative parsing).
// Constraint: restore only valid when head segment unchanged since save.
typedef struct XrArenaState {
    XrArenaSegment *head;
    char *position;
    size_t total_allocated;
} XrArenaState;

XR_FUNC XrArenaState xr_arena_save(XrArena *arena);
XR_FUNC void xr_arena_restore(XrArena *arena, XrArenaState state);

// Statistics
typedef struct XrArenaStats {
    size_t segment_count;
    size_t total_capacity;
    size_t total_used;
} XrArenaStats;

XR_FUNC void xr_arena_get_stats(XrArena *arena, XrArenaStats *stats);

// Convenience macros
#define xr_arena_new(arena, Type) ((Type *) xr_arena_alloc(arena, sizeof(Type)))

#define xr_arena_array(arena, Type, count) ((Type *) xr_arena_alloc_array(arena, sizeof(Type), (count)))

#endif  // XARENA_H
