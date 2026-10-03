/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarena.h - Shared bump allocator with explicit storage ownership
 */
#ifndef XARENA_H
#define XARENA_H

#include "xarena_backing.h"

#define XR_ARENA_SEGMENT_SIZE (64 * 1024)
#define XR_ARENA_ALIGNMENT 8

typedef struct XrArenaSegment XrArenaSegment;

/* Zero-initialize before first open. An open arena cannot be copied or moved.
 * First failure is sticky until destroy; even failed open must be destroyed.
 * The copied backing retains context until all segments are physically freed. */
typedef struct XrArena {
    XrArenaSegment *head;
    char *position;
    char *limit;
    size_t total_allocated;
    size_t segment_count;
    size_t total_capacity;
    uint64_t segment_serial;
    XrArenaBacking backing;
    XrArenaStatus status;
    bool retained;
} XrArena;

XR_FUNC XrArenaStatus xr_arena_open(XrArena *arena, size_t initial_size, const XrArenaBacking *backing);
XR_FUNC XrArenaStatus xr_arena_status(const XrArena *arena);
XR_FUNC void *xr_arena_alloc(XrArena *arena, size_t size);
XR_FUNC void *xr_arena_alloc_array(XrArena *arena, size_t elem_size, size_t count);
XR_FUNC void *xr_arena_alloc_raw(XrArena *arena, size_t size);
XR_FUNC void xr_arena_destroy(XrArena *arena);
XR_FUNC void xr_arena_reset(XrArena *arena);
XR_FUNC char *xr_arena_strdup(XrArena *arena, const char *str);
XR_FUNC char *xr_arena_strndup(XrArena *arena, const char *str, size_t len);
XR_FUNC size_t xr_arena_get_allocated_size(XrArena *arena);

/* Savepoints belong to one arena and one surviving allocation history.
 * Destroy/reset invalidate them. Restore frees newer segments physically.
 * Failed arenas neither save nor restore; their original failure survives. */
typedef struct XrArenaState {
    const XrArena *owner;
    XrArenaSegment *head;
    char *position;
    size_t total_allocated;
    uint64_t segment_serial;
} XrArenaState;

XR_FUNC XrArenaState xr_arena_save(XrArena *arena);
XR_FUNC void xr_arena_restore(XrArena *arena, XrArenaState state);

typedef struct XrArenaStats {
    size_t segment_count;
    size_t total_capacity;
    size_t total_used;
} XrArenaStats;

XR_FUNC void xr_arena_get_stats(XrArena *arena, XrArenaStats *stats);

#define xr_arena_new(arena, Type) ((Type *) xr_arena_alloc(arena, sizeof(Type)))
#define xr_arena_array(arena, Type, count) ((Type *) xr_arena_alloc_array(arena, sizeof(Type), (count)))

#endif // XARENA_H
