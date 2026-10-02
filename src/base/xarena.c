/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarena.c - One segment algorithm for explicitly owned storage domains
 */
#include "xarena.h"
#include <string.h>

struct XrArenaSegment {
    struct XrArenaSegment *next;
    size_t size;
    size_t capacity;
    uint64_t serial;
};

_Static_assert(sizeof(XrArenaSegment) % XR_ARENA_ALIGNMENT == 0,
    "Segment data must preserve arena alignment");
_Static_assert(sizeof(size_t) <= sizeof(uint64_t), "Work bytes must fit the backing counter");

static char *arena_segment_data(XrArenaSegment *segment) {
    return (char *) (segment + 1);
}

static XrArenaStatus arena_fail(XrArena *arena, XrArenaStatus status) {
    if (!arena) return XR_ARENA_BAD_ARGUMENT;
    if (arena->status == XR_ARENA_OK) arena->status = status;
    return arena->status;
}

static bool arena_ready(XrArena *arena) {
    if (!arena || arena->status != XR_ARENA_OK) return false;
    if (!arena->retained || !arena->head) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return false;
    }
    /* An explicit state backing may have failed through another owner. Query
     * before raw bump/save/reset too; zero units perform no ledger refund. */
    return arena_fail(arena, arena->backing.work(arena->backing.context, 0)) == XR_ARENA_OK;
}

static bool arena_work(XrArena *arena, size_t units) {
    XrArenaStatus status = arena->backing.work(arena->backing.context, (uint64_t) units);
    return arena_fail(arena, status) == XR_ARENA_OK;
}

static bool arena_align(XrArena *arena, size_t size, size_t *aligned) {
    if (size > SIZE_MAX - (XR_ARENA_ALIGNMENT - 1)) {
        arena_fail(arena, XR_ARENA_BUDGET);
        return false;
    }
    *aligned = (size + XR_ARENA_ALIGNMENT - 1) & ~((size_t) XR_ARENA_ALIGNMENT - 1);
    return true;
}

static XrArenaSegment *arena_segment(XrArena *arena, size_t capacity) {
    if (capacity > SIZE_MAX - sizeof(XrArenaSegment) ||
        capacity > SIZE_MAX - arena->total_capacity ||
        arena->segment_count == SIZE_MAX || arena->segment_serial == UINT64_MAX) {
        arena_fail(arena, XR_ARENA_BUDGET);
        return NULL;
    }
    void *memory = NULL;
    XrArenaStatus status = arena->backing.alloc(arena->backing.context,
        sizeof(XrArenaSegment) + capacity, &memory);
    if (arena_fail(arena, status) != XR_ARENA_OK) return NULL;
    XrArenaSegment *segment = memory;
    segment->next = arena->head;
    segment->size = 0;
    segment->capacity = capacity;
    segment->serial = ++arena->segment_serial;
    ++arena->segment_count;
    arena->total_capacity += capacity;
    arena->head = segment;
    arena->position = arena_segment_data(segment);
    arena->limit = arena->position + capacity;
    return segment;
}

XR_FUNC XrArenaStatus xr_arena_open(XrArena *arena, size_t initial_size, const XrArenaBacking *backing) {
    if (!arena) return XR_ARENA_BAD_ARGUMENT;
    if (arena->status != XR_ARENA_OK) return arena->status;
    if (arena->retained || !backing || !backing->alloc || !backing->free ||
        !backing->work || !backing->retain || !backing->release)
        return arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
    arena->backing = *backing;
    XrArenaStatus status = arena->backing.retain(arena->backing.context);
    if (arena_fail(arena, status) != XR_ARENA_OK) return arena->status;
    arena->retained = true;
    if (!initial_size) initial_size = XR_ARENA_SEGMENT_SIZE;
    if (arena_align(arena, initial_size, &initial_size)) arena_segment(arena, initial_size);
    return arena->status;
}

XR_FUNC XrArenaStatus xr_arena_status(const XrArena *arena) {
    return arena ? arena->status : XR_ARENA_BAD_ARGUMENT;
}

static void *arena_bump(XrArena *arena, size_t size) {
    if (!arena_ready(arena)) return NULL;
    if (!size) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return NULL;
    }
    if (!arena_align(arena, size, &size)) return NULL;
    if (size > SIZE_MAX - arena->total_allocated) {
        arena_fail(arena, XR_ARENA_BUDGET);
        return NULL;
    }
    if (size > (size_t) (arena->limit - arena->position)) {
        size_t capacity = size > XR_ARENA_SEGMENT_SIZE ? size : XR_ARENA_SEGMENT_SIZE;
        if (!arena_segment(arena, capacity)) return NULL;
    }
    void *memory = arena->position;
    arena->position += size;
    arena->head->size += size;
    arena->total_allocated += size;
    return memory;
}

XR_FUNC void *xr_arena_alloc_raw(XrArena *arena, size_t size) {
    return arena_bump(arena, size);
}

XR_FUNC void *xr_arena_alloc(XrArena *arena, size_t size) {
    void *memory = arena_bump(arena, size);
    if (!memory || !arena_work(arena, size)) return NULL;
    memset(memory, 0, size);
    return memory;
}

XR_FUNC void *xr_arena_alloc_array(XrArena *arena, size_t elem_size, size_t count) {
    if (!arena_ready(arena)) return NULL;
    if (!elem_size || !count) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return NULL;
    }
    if (count > SIZE_MAX / elem_size) {
        arena_fail(arena, XR_ARENA_BUDGET);
        return NULL;
    }
    return xr_arena_alloc(arena, elem_size * count);
}

static void arena_free_head(XrArena *arena) {
    XrArenaSegment *segment = arena->head;
    arena->head = segment->next;
    --arena->segment_count;
    arena->total_capacity -= segment->capacity;
    arena->total_allocated -= segment->size;
    arena->backing.free(arena->backing.context, segment);
}

XR_FUNC void xr_arena_destroy(XrArena *arena) {
    if (!arena) return;
    while (arena->head) arena_free_head(arena);
    XrArenaBacking backing = arena->backing;
    bool retained = arena->retained;
    uint64_t serial = arena->segment_serial;
    *arena = (XrArena) {0};
    arena->segment_serial = serial;
    /* The context may contain the arena itself; do not touch it after release. */
    if (retained) backing.release(backing.context);
}

XR_FUNC void xr_arena_reset(XrArena *arena) {
    if (!arena_ready(arena)) return;
    if (arena->segment_serial == UINT64_MAX) {
        arena_fail(arena, XR_ARENA_BUDGET);
        return;
    }
    XrArenaSegment *head = arena->head;
    arena->head = head->next;
    while (arena->head) arena_free_head(arena);
    arena->head = head;
    head->next = NULL;
    head->size = 0;
    head->serial = ++arena->segment_serial;
    arena->position = arena_segment_data(head);
    arena->limit = arena->position + head->capacity;
    arena->total_allocated = 0;
}

XR_FUNC char *xr_arena_strdup(XrArena *arena, const char *str) {
    if (!arena_ready(arena)) return NULL;
    if (!str) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return NULL;
    }
    size_t length = 0;
    for (;;) {
        if (!arena_work(arena, 1)) return NULL;
        if (!str[length]) break;
        if (length == SIZE_MAX - 1) {
            arena_fail(arena, XR_ARENA_BUDGET);
            return NULL;
        }
        ++length;
    }
    char *copy = arena_bump(arena, length + 1);
    if (!copy || !arena_work(arena, length + 1)) return NULL;
    memcpy(copy, str, length + 1);
    return copy;
}

XR_FUNC char *xr_arena_strndup(XrArena *arena, const char *str, size_t len) {
    if (!arena_ready(arena)) return NULL;
    if (!str) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return NULL;
    }
    if (len == SIZE_MAX) {
        arena_fail(arena, XR_ARENA_BUDGET);
        return NULL;
    }
    char *copy = arena_bump(arena, len + 1);
    if (!copy || !arena_work(arena, len)) return NULL;
    if (len) memcpy(copy, str, len);
    if (!arena_work(arena, 1)) return NULL;
    copy[len] = '\0';
    return copy;
}

XR_FUNC size_t xr_arena_get_allocated_size(XrArena *arena) {
    return arena ? arena->total_allocated : 0;
}

XR_FUNC XrArenaState xr_arena_save(XrArena *arena) {
    if (!arena_ready(arena)) return (XrArenaState) {0};
    return (XrArenaState) {arena, arena->head, arena->position,
        arena->total_allocated, arena->head->serial};
}

XR_FUNC void xr_arena_restore(XrArena *arena, XrArenaState state) {
    if (!arena_ready(arena)) return;
    if (state.owner != arena || !state.head) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return;
    }
    XrArenaSegment *segment = arena->head;
    size_t newer_used = 0;
    while (segment) {
        if (!arena_work(arena, 1)) return;
        if (segment == state.head) break;
        newer_used += segment->size;
        segment = segment->next;
    }
    if (!segment || segment->serial != state.segment_serial ||
        (uintptr_t) state.position < (uintptr_t) arena_segment_data(segment) ||
        (uintptr_t) state.position > (uintptr_t) (arena_segment_data(segment) + segment->size)) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return;
    }
    size_t used = (size_t) ((uintptr_t) state.position - (uintptr_t) arena_segment_data(segment));
    size_t total = arena->total_allocated - newer_used - segment->size + used;
    if (used % XR_ARENA_ALIGNMENT || total != state.total_allocated) {
        arena_fail(arena, XR_ARENA_BAD_ARGUMENT);
        return;
    }
    while (arena->head != segment) arena_free_head(arena);
    arena->position = state.position;
    arena->limit = arena_segment_data(segment) + segment->capacity;
    arena->total_allocated = total;
    segment->size = used;
}

XR_FUNC void xr_arena_get_stats(XrArena *arena, XrArenaStats *stats) {
    if (!stats) return;
    *stats = arena ? (XrArenaStats) {arena->segment_count, arena->total_capacity, arena->total_allocated}
                   : (XrArenaStats) {0};
}
