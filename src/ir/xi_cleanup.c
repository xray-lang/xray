/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cleanup.c - Verified static cleanup identity graph construction
 */

#include "xi_cleanup.h"

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"

#include <stdio.h>
#include <string.h>

typedef struct CleanupLocation {
    const XiBlock *block;
    uint32_t index;
} CleanupLocation;

static void cleanup_clear_error(char *error, size_t error_size) {
    if (error && error_size != 0u)
        error[0] = '\0';
}

static bool cleanup_fail(char *error, size_t error_size, const char *message) {
    if (error && error_size != 0u)
        (void) snprintf(error, error_size, "%s", message);
    return false;
}

static bool cleanup_is_marker(const XiValue *value) {
    return value->op == XI_CLEANUP_ENTER || value->op == XI_CLEANUP_LEAVE;
}

/* Pointer equality against live instructions must precede reading candidate.
 * An arena allocation can outlive removal of its instruction from the graph. */
static bool cleanup_find_live(const XiFunc *function, const XiValue *candidate,
                              CleanupLocation *location) {
    if (!function || !candidate || (function->nblocks != 0u && !function->blocks))
        return false;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!block || (block->nvalues != 0u && !block->values))
            continue;
        for (uint32_t index = 0u; index < block->nvalues; ++index) {
            if (block->values[index] != candidate)
                continue;
            if (candidate->block != block || block->func != function)
                return false;
            if (location)
                *location = (CleanupLocation) {.block = block, .index = index};
            return true;
        }
    }
    return false;
}

static bool cleanup_marker_shape(const XiFunc *function, const XiValue *value, uint16_t operation,
                                 CleanupLocation *location) {
    return cleanup_find_live(function, value, location) && value->op == operation &&
           value->nargs == 0u && value->type && value->type->kind == XR_KIND_UNIT &&
           (value->flags & XI_FLAG_SIDE_EFFECT) != 0u;
}

static bool cleanup_mapping_value_is_live(const XiFunc *function, const XiValue *candidate) {
    if (!function || !candidate)
        return false;
    if (cleanup_find_live(function, candidate, NULL))
        return true;
    for (uint16_t index = 0u; function->params && index < function->nparams; ++index) {
        if (function->params[index] == candidate)
            return true;
    }
    for (uint32_t block_index = 0u; function->blocks && block_index < function->nblocks;
         ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        for (const XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next) {
            if (&phi->value == candidate)
                return candidate->block == block && block->func == function;
        }
    }
    return false;
}

static bool cleanup_members_valid(const XiFunc *function, const XiCleanupBoundary *boundary,
                                  char *error, size_t error_size) {
    CleanupLocation enter = {0};
    CleanupLocation leave = {0};
    CleanupLocation next = {0};
    CleanupLocation frontier = {0};
    if (boundary->kind != XI_CLEANUP_BOUNDARY_CLOSED && boundary->kind != XI_CLEANUP_BOUNDARY_FATAL)
        return cleanup_fail(error, error_size, "cleanup boundary has no completion kind");
    if (boundary->rank == 0u || boundary->reserved[0] != 0u || boundary->reserved[1] != 0u ||
        boundary->reserved[2] != 0u)
        return cleanup_fail(error, error_size,
                            "cleanup boundary has invalid rank or reserved data");
    if (!cleanup_marker_shape(function, boundary->enter, XI_CLEANUP_ENTER, &enter) ||
        !cleanup_marker_shape(function, boundary->frontier, XI_CLEANUP_ENTER, &frontier))
        return cleanup_fail(error, error_size,
                            "cleanup enter or frontier is not a live local marker");
    if (boundary->kind == XI_CLEANUP_BOUNDARY_CLOSED) {
        if (!cleanup_marker_shape(function, boundary->leave, XI_CLEANUP_LEAVE, &leave) ||
            (enter.block == leave.block && enter.index >= leave.index))
            return cleanup_fail(error, error_size, "closed cleanup has no ordered live leave");
    } else if (boundary->leave) {
        return cleanup_fail(error, error_size, "fatal cleanup must not claim a normal leave");
    }
    if (frontier.block == enter.block && frontier.index > enter.index)
        return cleanup_fail(error, error_size, "cleanup frontier head follows its member");
    if (!boundary->remaining) {
        if (boundary->rank != 1u)
            return cleanup_fail(error, error_size, "cleanup frontier ends before rank one");
    } else {
        if (boundary->rank == 1u || boundary->remaining == boundary->enter ||
            !cleanup_marker_shape(function, boundary->remaining, XI_CLEANUP_ENTER, &next))
            return cleanup_fail(error, error_size,
                                "cleanup remaining identity is not a later item");
        if (boundary->leave && leave.block == next.block && leave.index >= next.index)
            return cleanup_fail(error, error_size,
                                "cleanup remaining item precedes the paired leave");
    }
    return true;
}

static const XiCleanupBoundary *cleanup_read_record(const XiFunc *function, const XiValue *value,
                                                    char *error, size_t error_size) {
    if (!cleanup_find_live(function, value, NULL)) {
        (void) cleanup_fail(error, error_size, "cleanup marker is not live in its function");
        return NULL;
    }
    const XiCleanupBoundary *boundary = value->cleanup_boundary;
    if (!xi_func_arena_contains(function, boundary, (uint32_t) sizeof(*boundary)) ||
        (uintptr_t) boundary % _Alignof(XiCleanupBoundary) != 0u) {
        (void) cleanup_fail(error, error_size, "cleanup record is not owned by its function arena");
        return NULL;
    }
    if (!cleanup_members_valid(function, boundary, error, error_size))
        return NULL;
    if ((value->op == XI_CLEANUP_ENTER && boundary->enter != value) ||
        (value->op == XI_CLEANUP_LEAVE && boundary->leave != value) || !cleanup_is_marker(value) ||
        boundary->enter->cleanup_boundary != boundary ||
        (boundary->leave && boundary->leave->cleanup_boundary != boundary)) {
        (void) cleanup_fail(error, error_size,
                            "cleanup marker pair does not share its exact record");
        return NULL;
    }
    return boundary;
}

static bool cleanup_count_records(const XiFunc *function, uint32_t *count, char *error,
                                  size_t error_size) {
    *count = 0u;
    if (!function || (function->nblocks != 0u && !function->blocks) ||
        (function->nparams != 0u && !function->params))
        return cleanup_fail(error, error_size, "cleanup verification has no complete function");
    for (uint16_t index = 0u; index < function->nparams; ++index) {
        if (!function->params[index] || function->params[index]->cleanup_boundary ||
            cleanup_is_marker(function->params[index]))
            return cleanup_fail(error, error_size, "function parameter carries cleanup identity");
    }
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!block || block->func != function || (block->nvalues != 0u && !block->values))
            return cleanup_fail(error, error_size, "cleanup verification has an invalid block");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (phi->value.cleanup_boundary || cleanup_is_marker(&phi->value))
                return cleanup_fail(error, error_size, "phi carries cleanup identity");
        }
        for (uint32_t index = 0u; index < block->nvalues; ++index) {
            const XiValue *value = block->values[index];
            if (!value)
                continue;
            if (!cleanup_is_marker(value)) {
                if (value->cleanup_boundary)
                    return cleanup_fail(error, error_size,
                                        "ordinary instruction carries cleanup identity");
                continue;
            }
            if (!cleanup_read_record(function, value, error, error_size))
                return false;
            if (value->op == XI_CLEANUP_ENTER) {
                if (*count == UINT32_MAX)
                    return cleanup_fail(error, error_size,
                                        "cleanup boundary count exceeds its limit");
                ++*count;
            }
        }
    }
    return true;
}

static uint32_t cleanup_find_record(const XiCleanupBoundary *const *records, uint32_t count,
                                    const XiValue *enter) {
    for (uint32_t index = 0u; index < count; ++index) {
        if (records[index]->enter == enter)
            return index;
    }
    return UINT32_MAX;
}

/* Each head walks one decreasing-rank chain. Visitation is global, so forks,
 * duplicate rows, missing heads and cross-frontier joins cannot masquerade as
 * several individually plausible successor relations. */
static bool cleanup_graph_valid(const XiCleanupBoundary *const *records, uint32_t count,
                                char *error, size_t error_size) {
    if (count == 0u)
        return true;
    uint8_t *visited = xr_calloc(count, sizeof(*visited));
    if (!visited)
        return cleanup_fail(error, error_size, "cleanup graph validation allocation failed");
    bool valid = true;
    for (uint32_t index = 0u; index < count && valid; ++index) {
        const XiCleanupBoundary *head = records[index];
        if (head->rank > count) {
            valid =
                cleanup_fail(error, error_size, "cleanup rank exceeds the complete boundary set");
            break;
        }
        if (head->frontier != head->enter)
            continue;
        const XiValue *current = head->enter;
        for (uint32_t rank = head->rank; rank > 0u; --rank) {
            uint32_t member = cleanup_find_record(records, count, current);
            if (member == UINT32_MAX || visited[member] || records[member]->rank != rank ||
                records[member]->frontier != head->enter) {
                valid = cleanup_fail(error, error_size,
                                     "cleanup frontier is cyclic, incomplete or cross-linked");
                break;
            }
            visited[member] = 1u;
            current = records[member]->remaining;
        }
        if (valid && current)
            valid = cleanup_fail(error, error_size,
                                 "cleanup rank does not end at its frontier boundary");
    }
    for (uint32_t index = 0u; index < count && valid; ++index) {
        if (!visited[index])
            valid = cleanup_fail(error, error_size,
                                 "cleanup item is not owned by exactly one frontier head");
    }
    xr_free(visited);
    return valid;
}

XR_FUNC bool xi_cleanup_boundary_attach(XiFunc *function, const XiCleanupBoundary *boundary,
                                        char *error, size_t error_size) {
    cleanup_clear_error(error, error_size);
    if (!function || !boundary)
        return cleanup_fail(error, error_size, "cleanup attachment has no function or record");
    if (!cleanup_members_valid(function, boundary, error, error_size))
        return false;
    if (boundary->enter->cleanup_boundary || (boundary->leave && boundary->leave->cleanup_boundary))
        return cleanup_fail(error, error_size,
                            "cleanup attachment would replace an existing identity");
    XiCleanupBoundary *owned = xi_func_arena_alloc(function, (uint32_t) sizeof(*owned));
    if (!owned)
        return cleanup_fail(error, error_size, "cleanup record allocation failed");
    *owned = *boundary;
    owned->enter->cleanup_boundary = owned;
    if (owned->leave)
        owned->leave->cleanup_boundary = owned;
    return true;
}

XR_FUNC bool xi_cleanup_verify(const XiFunc *function, char *error, size_t error_size) {
    cleanup_clear_error(error, error_size);
    uint32_t count = 0u;
    if (!cleanup_count_records(function, &count, error, error_size))
        return false;
    if (count == 0u)
        return true;
    const XiCleanupBoundary **records = xr_calloc(count, sizeof(*records));
    if (!records)
        return cleanup_fail(error, error_size, "cleanup boundary inventory allocation failed");
    uint32_t cursor = 0u;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        for (uint32_t index = 0u; index < block->nvalues; ++index) {
            const XiValue *value = block->values[index];
            if (value && value->op == XI_CLEANUP_ENTER) {
                XR_DCHECK(cursor < count, "cleanup inventory count changed during verification");
                records[cursor++] = value->cleanup_boundary;
            }
        }
    }
    bool valid = cleanup_graph_valid(records, count, error, error_size);
    xr_free(records);
    return valid;
}

static XiValue *cleanup_mapped_value(const XiCleanupRemap *remap, const XiValue *source) {
    for (uint32_t index = 0u; index < remap->count; ++index) {
        if (remap->values[index].source == source)
            return remap->values[index].target;
    }
    return NULL;
}

static bool cleanup_map_valid(const XiFunc *target, const XiFunc *source,
                              const XiCleanupRemap *remap, char *error, size_t error_size) {
    if (!target || !source || !remap || (remap->count != 0u && !remap->values))
        return cleanup_fail(error, error_size, "cleanup clone has no complete mapping input");
    for (uint32_t index = 0u; index < remap->count; ++index) {
        const XiCleanupValueMapping *entry = &remap->values[index];
        if (!cleanup_mapping_value_is_live(source, entry->source) ||
            !cleanup_mapping_value_is_live(target, entry->target))
            return cleanup_fail(error, error_size,
                                "cleanup clone mapping contains a non-live value");
        for (uint32_t previous = 0u; previous < index; ++previous) {
            if (remap->values[previous].source == entry->source)
                return cleanup_fail(error, error_size,
                                    "cleanup clone maps one source more than once");
            if (cleanup_is_marker(entry->source) && remap->values[previous].target == entry->target)
                return cleanup_fail(error, error_size, "cleanup clone merges marker identities");
        }
        if (cleanup_is_marker(entry->source)) {
            if (entry->source == entry->target || entry->source->op != entry->target->op ||
                entry->target->cleanup_boundary)
                return cleanup_fail(error, error_size,
                                    "cleanup clone target is not a fresh matching marker");
            if (!cleanup_read_record(source, entry->source, error, error_size))
                return false;
        } else if (entry->source->cleanup_boundary || cleanup_is_marker(entry->target) ||
                   entry->target->cleanup_boundary) {
            return cleanup_fail(error, error_size,
                                "cleanup clone maps ordinary data to boundary identity");
        }
    }
    return true;
}

static bool cleanup_clone_shape(const XiFunc *target, const XiCleanupBoundary *source,
                                const XiCleanupRemap *remap, XiCleanupBoundary *draft, char *error,
                                size_t error_size) {
    *draft = *source;
    draft->enter = cleanup_mapped_value(remap, source->enter);
    draft->frontier = cleanup_mapped_value(remap, source->frontier);
    draft->leave = source->leave ? cleanup_mapped_value(remap, source->leave) : NULL;
    draft->remaining = source->remaining ? cleanup_mapped_value(remap, source->remaining) : NULL;
    if (!draft->enter || !draft->frontier || (source->leave && !draft->leave) ||
        (source->remaining && !draft->remaining))
        return cleanup_fail(error, error_size,
                            "cleanup clone omitted a pair, frontier or remaining mapping");
    return cleanup_members_valid(target, draft, error, error_size);
}

static bool cleanup_clone_records(XiFunc *target, const XiCleanupRemap *remap,
                                  const XiCleanupBoundary *const *records, uint32_t count,
                                  char *error, size_t error_size) {
    if (count == 0u)
        return true;
    XiCleanupBoundary *drafts = xr_calloc(count, sizeof(*drafts));
    XiCleanupBoundary **prepared = xr_calloc(count, sizeof(*prepared));
    if (!drafts || !prepared) {
        xr_free(prepared);
        xr_free(drafts);
        return cleanup_fail(error, error_size, "cleanup clone staging allocation failed");
    }
    bool valid = true;
    for (uint32_t index = 0u; index < count && valid; ++index)
        valid =
            cleanup_clone_shape(target, records[index], remap, &drafts[index], error, error_size);
    for (uint32_t index = 0u; index < count && valid; ++index) {
        prepared[index] = xi_func_arena_alloc(target, (uint32_t) sizeof(*prepared[index]));
        if (!prepared[index]) {
            valid = cleanup_fail(error, error_size, "cleanup clone record allocation failed");
            break;
        }
        *prepared[index] = drafts[index];
    }
    if (valid) {
        for (uint32_t index = 0u; index < count; ++index) {
            XiCleanupBoundary *boundary = prepared[index];
            XR_DCHECK(boundary != NULL, "cleanup clone published an unprepared record");
            boundary->enter->cleanup_boundary = boundary;
            if (boundary->leave)
                boundary->leave->cleanup_boundary = boundary;
        }
    }
    xr_free(prepared);
    xr_free(drafts);
    return valid;
}

XR_FUNC bool xi_cleanup_remap(XiFunc *target, const XiFunc *source, const XiCleanupRemap *remap,
                              char *error, size_t error_size) {
    cleanup_clear_error(error, error_size);
    if (!cleanup_map_valid(target, source, remap, error, error_size))
        return false;
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < remap->count; ++index)
        count += remap->values[index].source->op == XI_CLEANUP_ENTER;
    const XiCleanupBoundary **records = count ? xr_calloc(count, sizeof(*records)) : NULL;
    if (count != 0u && !records)
        return cleanup_fail(error, error_size, "cleanup source subset allocation failed");
    uint32_t cursor = 0u;
    bool valid = true;
    for (uint32_t index = 0u; index < remap->count; ++index) {
        const XiValue *value = remap->values[index].source;
        if (!cleanup_is_marker(value))
            continue;
        const XiCleanupBoundary *boundary = value->cleanup_boundary;
        if (!cleanup_mapped_value(remap, boundary->enter) ||
            (boundary->leave && !cleanup_mapped_value(remap, boundary->leave))) {
            valid =
                cleanup_fail(error, error_size, "cleanup clone contains an incomplete marker pair");
            break;
        }
        if (value->op == XI_CLEANUP_ENTER) {
            XR_DCHECK(cursor < count, "cleanup clone source count changed during collection");
            records[cursor++] = boundary;
        }
    }
    if (valid)
        valid = cleanup_graph_valid(records, count, error, error_size);
    if (valid)
        valid = cleanup_clone_records(target, remap, records, count, error, error_size);
    xr_free(records);
    return valid;
}
