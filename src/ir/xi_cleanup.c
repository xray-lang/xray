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
#include "xi_coro_lower.h"
#include "xi_edit.h"
#include "xi_verify.h"

#include "../analysis/xglobal_summary.h"
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

static uint32_t cleanup_block_index(const XiFunc *function, const XiBlock *block) {
    for (uint32_t i = 0u; i < function->nblocks; ++i)
        if (function->blocks[i] == block)
            return i;
    return UINT32_MAX;
}

XR_FUNC bool xi_try_region_reaches_point(const XiFunc *f, const XiValue *try_op,
                                          const XiValue *point) {
    if (!f || !try_op || !try_op->block || !point || !point->block)
        return true;
    uint32_t try_index = try_op->block->nvalues;
    for (uint32_t i = 0; i < try_op->block->nvalues; i++) {
        if (try_op->block->values[i] == try_op) {
            try_index = i;
            break;
        }
    }
    if (try_index == try_op->block->nvalues)
        return true;

    uint8_t *visited = (uint8_t *) xr_calloc(f->nblocks, sizeof(uint8_t));
    XiBlock **queue = (XiBlock **) xr_calloc(f->nblocks, sizeof(XiBlock *));
    if (!visited || !queue) {
        xr_free(visited);
        xr_free(queue);
        return true;
    }
    uint32_t head = 0, tail = 0;
    uint32_t try_bi = cleanup_block_index(f, try_op->block);
    if (try_bi == UINT32_MAX) {
        xr_free(visited);
        xr_free(queue);
        return true;
    }
    visited[try_bi] = 1;

    bool stopped = false;
    for (uint32_t i = try_index + 1; i < try_op->block->nvalues; i++) {
        XiValue *value = try_op->block->values[i];
        if (value == point) {
            xr_free(visited);
            xr_free(queue);
            return true;
        }
        if (value && value->op == XI_END_TRY && value->aux == try_op) {
            stopped = true;
            break;
        }
    }
    if (!stopped) {
        for (uint32_t s = 0; s < 2; s++) {
            XiBlock *succ = try_op->block->succs[s];
            uint32_t succ_bi = cleanup_block_index(f, succ);
            if (succ && succ_bi != UINT32_MAX && !visited[succ_bi] && tail < f->nblocks) {
                visited[succ_bi] = 1;
                queue[tail++] = succ;
            }
        }
    }
    bool found = false;
    while (head < tail && !found) {
        XiBlock *block = queue[head++];
        bool ends_region = false;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            XiValue *value = block->values[i];
            if (value == point) {
                found = true;
                break;
            }
            if (value && value->op == XI_END_TRY && value->aux == try_op) {
                ends_region = true;
                break;
            }
        }
        if (found || ends_region)
            continue;
        for (uint32_t s = 0; s < 2; s++) {
            XiBlock *succ = block->succs[s];
            uint32_t succ_bi = cleanup_block_index(f, succ);
            if (succ && succ_bi != UINT32_MAX && !visited[succ_bi] && tail < f->nblocks) {
                visited[succ_bi] = 1;
                queue[tail++] = succ;
            }
        }
    }
    xr_free(visited);
    xr_free(queue);
    return found;
}

XR_FUNC bool xi_value_has_panic_continuation(const XiFunc *function, const XiValue *call) {
    if (!function || !call)
        return false;
    for (uint32_t b = 0u; b < function->nblocks; ++b) {
        const XiBlock *block = function->blocks[b];
        for (uint32_t i = 0u; i < block->nvalues; ++i) {
            const XiValue *value = block->values[i];
            if (value->op == XI_CATCH && value->aux_int == XI_CATCH_AUX_POINT_PANIC &&
                value->aux == call)
                return true;
        }
    }
    return false;
}

typedef struct CleanupGraphClone {
    XiFunc *function;
    uint32_t block_count;
    uint32_t value_count;
    XiBlock **blocks;
    XiValue **values;
    uint8_t *members;
    XiCleanupValueMapping *mapping;
    uint32_t mapping_count;
    const XiErrorRegion **source_regions;
    XiErrorRegion **target_regions;
    uint32_t region_count;
} CleanupGraphClone;

static XiValue *cleanup_clone_value(const CleanupGraphClone *clone, const XiValue *source) {
    if (!source)
        return NULL;
    return source->id < clone->value_count && clone->values[source->id]
               ? clone->values[source->id] : (XiValue *) source;
}

static XiBlock *cleanup_clone_block(const CleanupGraphClone *clone, const XiBlock *source) {
    if (!source)
        return NULL;
    return source->id < clone->block_count && clone->blocks[source->id]
               ? clone->blocks[source->id] : (XiBlock *) source;
}

static void cleanup_clone_dispose(CleanupGraphClone *clone) {
    xr_free(clone->blocks);
    xr_free(clone->values);
    xr_free(clone->members);
    xr_free(clone->mapping);
    xr_free(clone->source_regions);
    xr_free(clone->target_regions);
    memset(clone, 0, sizeof(*clone));
}

static bool cleanup_clone_collect(CleanupGraphClone *clone, XiBlock *entry) {
    if (!entry || entry->id >= clone->block_count ||
        clone->function->blocks[entry->id] != entry)
        return false;
    clone->members[entry->id] = 1u;
    bool changed;
    do {
        changed = false;
        for (uint32_t b = 0u; b < clone->block_count; ++b) {
            if (!clone->members[b])
                continue;
            const XiBlock *block = clone->function->blocks[b];
            for (uint32_t i = 0u; i < block->nvalues + 2u; ++i) {
                const XiBlock *next = i < 2u ? block->succs[i] : NULL;
                const XiValue *value = i < 2u ? NULL : block->values[i - 2u];
                if (value && value->op == XI_TRY)
                    next = (const XiBlock *) value->aux;
                if (!next)
                    continue;
                uint32_t index = cleanup_block_index(clone->function, next);
                if (index >= clone->block_count)
                    return false;
                if (!clone->members[index]) {
                    clone->members[index] = 1u;
                    changed = true;
                }
            }
        }
    } while (changed);
    return true;
}

static bool cleanup_clone_create_values(CleanupGraphClone *clone, XiValue *registration,
                                         XiValue *incoming) {
    for (uint32_t b = 0u; b < clone->block_count; ++b) {
        if (!clone->members[b])
            continue;
        const XiBlock *source = clone->function->blocks[b];
        XiBlock *target = xi_block_new(clone->function);
        if (!target)
            return false;
        clone->blocks[b] = target;
        target->kind = source->kind;
        target->line = source->line;
        target->sealed = true;
        target->exit_reason = XI_EXIT_REASON_PANIC;
        if (source == registration->aux) {
            XiValue *end = xi_value_new(clone->function, target, XI_END_TRY,
                                        registration->type, 0u);
            if (!end)
                return false;
            end->aux = registration;
            end->flags |= XI_FLAG_SIDE_EFFECT;
        }
        for (const XiPhi *phi = source->phis; phi; phi = phi->next) {
            XiPhi *copy = xi_phi_new(clone->function, target, phi->value.type, phi->value.nargs);
            if (!copy)
                return false;
            clone->values[phi->value.id] = &copy->value;
            clone->mapping[clone->mapping_count++] =
                (XiCleanupValueMapping) {&phi->value, &copy->value};
        }
        for (uint32_t i = 0u; i < source->nvalues; ++i) {
            const XiValue *value = source->values[i];
            XiValue *copy = incoming && value->op == XI_CATCH && value->aux == registration
                                ? incoming
                                : xi_value_new(clone->function, target, value->op, value->type,
                                                 value->nargs);
            if (!copy)
                return false;
            clone->values[value->id] = copy;
            clone->mapping[clone->mapping_count++] = (XiCleanupValueMapping) {value, copy};
        }
    }
    return true;
}

static bool cleanup_clone_region(CleanupGraphClone *clone, const XiErrorRegion *source,
                                  XiErrorRegion **out) {
    *out = (XiErrorRegion *) source;
    if (!source || !source->registration_block ||
        source->registration_block->id >= clone->block_count ||
        !clone->members[source->registration_block->id])
        return true;
    for (uint32_t i = 0u; i < clone->region_count; ++i)
        if (clone->source_regions[i] == source) {
            *out = clone->target_regions[i];
            return true;
        }
    if (clone->region_count >= clone->value_count)
        return false;
    XiErrorRegion *target = xi_func_arena_alloc(clone->function, sizeof(*target));
    if (!target)
        return false;
    uint32_t index = clone->region_count++;
    clone->source_regions[index] = source;
    clone->target_regions[index] = target;
    *target = *source;
    target->registration_block = cleanup_clone_block(clone, source->registration_block);
    target->body_block = cleanup_clone_block(clone, source->body_block);
    target->catch_block = cleanup_clone_block(clone, source->catch_block);
    target->merge_block = cleanup_clone_block(clone, source->merge_block);
    target->catch_value = cleanup_clone_value(clone, source->catch_value);
    *out = target;
    return cleanup_clone_region(clone, source->parent, &target->parent);
}

static bool cleanup_clone_finish(CleanupGraphClone *clone, XiValue *incoming) {
    for (uint32_t i = 0u; i < clone->mapping_count; ++i) {
        const XiValue *source = clone->mapping[i].source;
        XiValue *target = clone->mapping[i].target;
        if (target == incoming)
            continue;
        if (!xi_value_clone_metadata(clone->function, target, source))
            return false;
        target->cleanup_boundary = NULL;
        for (uint16_t arg = 0u; arg < source->nargs; ++arg)
            target->args[arg] = cleanup_clone_value(clone, source->args[arg]);
        target->error_producer = cleanup_clone_value(clone, source->error_producer);
        if (!cleanup_clone_region(clone, source->error_region, &target->error_region))
            return false;
        if (source->op == XI_TRY)
            target->aux = cleanup_clone_block(clone, (const XiBlock *) source->aux);
        else if (source->op == XI_END_TRY || source->op == XI_CATCH)
            target->aux = cleanup_clone_value(clone, (const XiValue *) source->aux);
        if (!xi_value_clone_call_plan(clone->function, target, source))
            return false;
    }
    for (uint32_t b = 0u; b < clone->block_count; ++b) {
        if (!clone->members[b])
            continue;
        const XiBlock *source = clone->function->blocks[b];
        XiBlock *target = clone->blocks[b];
        target->control = cleanup_clone_value(clone, source->control);
        for (uint32_t edge = 0u; edge < 2u; ++edge)
            target->succs[edge] = cleanup_clone_block(clone, source->succs[edge]);
        for (uint16_t pred = 0u; pred < source->npreds; ++pred) {
            const XiBlock *prior = source->preds[pred];
            if (prior->id >= clone->block_count || !clone->members[prior->id])
                continue;
            if (!xi_block_add_pred(target, clone->blocks[prior->id]))
                return false;
        }
    }
    XiCleanupRemap remap = {clone->mapping, clone->mapping_count};
    return xi_cleanup_remap(clone->function, clone->function, &remap, NULL, 0u);
}

static bool cleanup_clone_graph(XiFunc *function, XiValue *registration, XiValue *incoming,
                                 CleanupGraphClone *clone) {
    *clone = (CleanupGraphClone) {.function = function, .block_count = function->nblocks,
                                  .value_count = function->next_value_id};
    clone->blocks = xr_calloc(clone->block_count, sizeof(*clone->blocks));
    clone->members = xr_calloc(clone->block_count, sizeof(*clone->members));
    clone->values = xr_calloc(clone->value_count, sizeof(*clone->values));
    clone->mapping = xr_calloc(clone->value_count, sizeof(*clone->mapping));
    clone->source_regions = xr_calloc(clone->value_count, sizeof(*clone->source_regions));
    clone->target_regions = xr_calloc(clone->value_count, sizeof(*clone->target_regions));
    return clone->blocks && clone->members && clone->values && clone->mapping &&
           clone->source_regions && clone->target_regions &&
           cleanup_clone_collect(clone, (XiBlock *) registration->aux) &&
           cleanup_clone_create_values(clone, registration, incoming) &&
           cleanup_clone_finish(clone, incoming);
}

static bool call_lexical_panic_handlers(const XiFunc *function, const XiValue *call,
                                        XiValue **handlers, uint32_t *count) {
    *count = 0u;
    for (uint32_t b = 0u; b < function->nblocks; ++b) {
        const XiBlock *block = function->blocks[b];
        for (uint32_t i = 0u; i < block->nvalues; ++i) {
            const XiValue *value = block->values[i];
            if (value->op != XI_TRY || !xi_try_region_reaches_point(function, value, call))
                continue;
            if (value->aux_int != XI_TRY_AUX_STATIC_CLEANUP)
                return false;
            uint32_t position = (*count)++;
            while (position && xi_try_region_reaches_point(function, value, handlers[position - 1u])) {
                handlers[position] = handlers[position - 1u];
                --position;
            }
            handlers[position] = (XiValue *) value;
        }
    }
    for (uint32_t i = 1u; i < *count; ++i)
        if (!xi_try_region_reaches_point(function, handlers[i - 1u], handlers[i]))
            return false;
    return true;
}

static bool normalize_call_cleanup_exit(XiFunc *function, const XiValue *call,
                                         XiValue *const *handlers, uint32_t count) {
    CleanupGraphClone previous = {0};
    XiValue *payload = NULL;
    bool valid = true;
    for (uint32_t remaining = count; remaining > 0u && valid; --remaining) {
        XiValue *registration = handlers[remaining - 1u];
        CleanupGraphClone current;
        valid = cleanup_clone_graph(function, registration, payload, &current);
        XiBlock *entry = valid ? cleanup_clone_block(&current, registration->aux) : NULL;
        if (valid && !payload) {
            for (uint32_t i = 0u; i < entry->nvalues; ++i) {
                XiValue *value = entry->values[i];
                if (value->op == XI_CATCH && value->aux == registration) {
                    payload = value;
                    value->aux = (void *) call;
                    value->aux_int = XI_CATCH_AUX_POINT_PANIC;
                    break;
                }
            }
            valid = payload != NULL;
            if (valid)
                valid = xi_block_add_pred(entry, call->block);
        } else if (valid) {
            uint32_t connected = 0u;
            for (uint32_t b = 0u; b < previous.block_count; ++b) {
                XiBlock *block = previous.blocks[b];
                if (!block || block->kind != XI_BLOCK_UNREACHABLE)
                    continue;
                for (uint32_t i = 0u; i < block->nvalues; ++i) {
                    XiValue *value = block->values[i];
                    if (value->op != XI_THROW || value->nargs != 1u || value->args[0] != payload)
                        continue;
                    memmove(&block->values[i], &block->values[i + 1u],
                            (block->nvalues - i - 1u) * sizeof(*block->values));
                    --block->nvalues;
                    block->control = NULL;
                    if (!xi_block_add_pred(entry, block)) {
                        valid = false;
                        break;
                    }
                    block->kind = XI_BLOCK_PLAIN;
                    block->succs[0] = entry;
                    block->succs[1] = NULL;
                    ++connected;
                    break;
                }
            }
            valid = valid && connected != 0u;
        }
        cleanup_clone_dispose(&previous);
        previous = current;
    }
    cleanup_clone_dispose(&previous);
    return valid;
}

/* A fresh fixed-length allocation keeps its bounds through pure work and
 * element writes. Any other side effect ends this local proof. This preserves
 * literal initialization prefixes while routing potentially failing accesses. */
static bool cleanup_index_bounds_proven(const XiValue *point) {
    if (!point->args || point->nargs < 2u)
        return false;
    const XiValue *array = point->args[0];
    const XiValue *index = point->args[1];
    if (!array || array->op != XI_ARRAY_NEW || array->nargs != 1u || !array->args ||
        !array->args[0] || array->args[0]->op != XI_CONST || !index ||
        index->op != XI_CONST || !index->type || index->type->kind != XR_KIND_INT ||
        index->aux_int < 0 || index->aux_int >= array->args[0]->aux_int ||
        !point->block || array->block != point->block)
        return false;
    bool allocated = false;
    for (uint32_t i = 0u; i < point->block->nvalues; ++i) {
        const XiValue *value = point->block->values[i];
        if (value == point)
            return allocated;
        if (value == array) {
            allocated = true;
            continue;
        }
        if (!allocated || !(value->flags & XI_FLAG_SIDE_EFFECT))
            continue;
        if (value->op == XI_INDEX_SET && value->nargs == 3u && value->args &&
            value->args[0] == array)
            continue;
        if (value->op != XI_RETAIN && value->op != XI_RELEASE)
            return false;
    }
    return false;
}

static bool cleanup_point_may_panic(const XiFunc *function, const XiValue *point,
                                     const XgGlobalEvidence *evidence) {
    if (point->op == XI_ASSERTION)
        return true;
    if (point->op == XI_INDEX_GET || point->op == XI_INDEX_SET)
        return !cleanup_index_bounds_proven(point);
    if (point->op == XI_DIV || point->op == XI_MOD)
        return point->type && point->type->kind == XR_KIND_INT;
    if (point->op != XI_CALL && point->op != XI_CALL_METHOD && point->op != XI_CALL_METHOD_DIRECT)
        return false;
    const XgCallsiteSummary *site =
        xg_global_evidence_find_callsite(evidence, point->xg_callsite_id);
    uint32_t required = XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_PANIC;
    return site && site->owner_func_id == function->xg_body_func_id &&
           (site->flags & required) == required;
}

static void cleanup_replace_predecessor(XiBlock *block, XiBlock *before, XiBlock *after) {
    for (uint16_t p = 0u; block && p < block->npreds; ++p)
        if (block->preds[p] == before)
            block->preds[p] = after;
}

/* Give a failure point its own incoming ownership frontier. Values dead in
 * the prefix then end there on every outcome; live values cross an ordinary
 * edge and are available to the point's normal and exceptional successors.
 * Keeping the point inside its old block would require each consumer to
 * reconstruct a different mid-block owner snapshot. */
static XiBlock *cleanup_split_before(XiFunc *function, XiBlock *before, uint32_t index) {
    if (index == 0u)
        return before;
    if (index > before->nvalues)
        return NULL;
    uint32_t count = before->nvalues - index;
    XiBlock *after = xi_block_new(function);
    if (!after || count > UINT32_MAX / sizeof(*after->values))
        return NULL;
    if (count > after->values_cap) {
        XiValue **values = xi_func_arena_alloc(function, count * sizeof(*values));
        if (!values)
            return NULL;
        after->values = values;
        after->values_cap = count;
    }
    after->kind = before->kind;
    after->control = before->control;
    after->succs[0] = before->succs[0];
    after->succs[1] = before->succs[1];
    after->exit_reason = before->exit_reason;
    after->line = count ? before->values[index]->line : before->line;
    after->sealed = before->sealed;
    after->preds[0] = before;
    after->npreds = 1u;
    for (uint32_t i = 0u; i < count; ++i) {
        XiValue *value = before->values[index + i];
        after->values[i] = value;
        value->block = after;
        if (value->op == XI_TRY && value->aux)
            cleanup_replace_predecessor(value->aux, before, after);
        if (value->error_region && value->error_region->registration_block == before)
            value->error_region->registration_block = after;
    }
    after->nvalues = count;
    cleanup_replace_predecessor(after->succs[0], before, after);
    cleanup_replace_predecessor(after->succs[1], before, after);
    before->nvalues = index;
    before->kind = XI_BLOCK_PLAIN;
    before->control = NULL;
    before->succs[0] = after;
    before->succs[1] = NULL;
    return after;
}

static bool normalize_function_panic_exits(XiFunc *function, const XgGlobalEvidence *evidence,
                                           struct XrVMRuntime *isolate, char *error,
                                           size_t error_size) {
    XiEditSession edit;
    if (!xi_edit_begin(&edit, function))
        return cleanup_fail(error, error_size, "panic exit edit session is unavailable");
    bool changed = false;
    uint32_t original_blocks = function->nblocks;
    for (uint32_t b = 0u; b < original_blocks; ++b) {
        XiBlock *source = function->blocks[b];
        uint32_t original_values = source->nvalues;
        for (uint32_t v = 0u; v < original_values; ++v) {
            const XiValue *call = source->values[v];
            if (!cleanup_point_may_panic(function, call, evidence) ||
                xi_value_has_panic_continuation(function, call))
                continue;
            XiValue **handlers = xr_calloc(function->next_value_id, sizeof(*handlers));
            if (!handlers)
                return cleanup_fail(error, error_size, "panic handler census allocation failed");
            uint32_t handler_count = 0u;
            bool exact = call_lexical_panic_handlers(function, call, handlers, &handler_count);
            /* Without lexical cleanup, the Program owner synthesizes the
             * ordinary failure edge. Avoid splitting a module-slot borrow
             * merely to add an empty handler. */
            if (!exact || (handler_count == 0u &&
                           (call->op == XI_INDEX_GET || call->op == XI_INDEX_SET))) {
                xr_free(handlers);
                continue;
            }
            source = cleanup_split_before(function, source, v);
            if (!source) {
                xr_free(handlers);
                return cleanup_fail(error, error_size, "panic ownership frontier allocation failed");
            }
            original_values = source->nvalues;
            v = 0u;
            const XgCallsiteSummary *site =
                (call->op == XI_CALL || call->op == XI_CALL_METHOD ||
                 call->op == XI_CALL_METHOD_DIRECT)
                    ? xg_global_evidence_find_callsite(evidence, call->xg_callsite_id) : NULL;
            bool has_error_check = false;
            for (uint32_t i = 1u; i < source->nvalues; ++i) {
                const XiValue *value = source->values[i];
                if ((value->op == XI_ERR_CHECK || value->op == XI_CLEANUP_ERR_CHECK) &&
                    value->error_producer == call) {
                    has_error_check = true;
                    break;
                }
            }
            /* A narrowed call summary does not remove an existing error-channel
             * consumer. Keep the constructive pair in one block until an owned
             * transformation removes the check and its error continuation. */
            if (site && (site->flags & XG_CALL_MAY_ERROR) == 0u && !has_error_check &&
                (source->nvalues != 1u || source->kind != XI_BLOCK_PLAIN || !source->succs[0])) {
                source = cleanup_split_before(function, source, 1u);
                if (!source) {
                    xr_free(handlers);
                    return cleanup_fail(error, error_size, "panic call continuation allocation failed");
                }
                original_values = source->nvalues;
                v = UINT32_MAX;
            }
            bool lowered = handler_count == 0u ||
                           normalize_call_cleanup_exit(function, call, handlers, handler_count);
            xr_free(handlers);
            if (!lowered)
                return cleanup_fail(error, error_size, "panic cleanup graph normalization failed");
            if (handler_count != 0u) {
                changed = true;
                continue;
            }
            struct XrType *panic_type = xr_type_new_named_instance(isolate, "PanicInfo");
            struct XrType *unit_type = xr_type_new_unit(isolate);
            if (!panic_type || !panic_type->instance.class_name || !unit_type)
                return cleanup_fail(error, error_size, "panic exit type allocation failed");
            XiBlock *handler = xi_block_new(function);
            if (!handler)
                return cleanup_fail(error, error_size, "panic exit block allocation failed");
            XiValue *caught = xi_value_new(function, handler, XI_CATCH, panic_type, 0u);
            XiValue *publish = xi_value_new(function, handler, XI_THROW, unit_type, 1u);
            if (!caught || !publish)
                return cleanup_fail(error, error_size, "panic exit payload allocation failed");
            caught->aux = (void *) call;
            caught->aux_int = XI_CATCH_AUX_POINT_PANIC;
            caught->flags |= XI_FLAG_SIDE_EFFECT;
            caught->line = call->line;
            publish->args[0] = caught;
            publish->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
            publish->line = call->line;
            handler->kind = XI_BLOCK_UNREACHABLE;
            handler->exit_reason = XI_EXIT_REASON_PANIC;
            handler->control = caught;
            handler->line = call->line;
            handler->sealed = true;
            if (!xi_block_add_pred(handler, call->block))
                return cleanup_fail(error, error_size, "panic predecessor allocation failed");
            changed = true;
        }
    }
    XiPassOutcome outcome;
    XiPassChange change = {.cfg_changed = changed, .values_changed = changed};
    if (!xi_edit_finish(&edit, change, 0u, 0u, &outcome, error, error_size))
        return false;
    if (changed && function->coro_plan && !xi_coro_plan_rebase(function))
        return cleanup_fail(error, error_size, "panic exit coroutine facts could not be rebased");
    return xi_verify_stage(function, function->stage, error, (int) error_size);
}

XR_FUNC bool xi_normalize_panic_exits(XiFunc *root, const XgGlobalEvidence *evidence,
                                            struct XrVMRuntime *isolate, char *error,
                                            size_t error_size) {
    if (!root || !evidence || !isolate || error_size > INT32_MAX)
        return cleanup_fail(error, error_size, "panic exit normalization input is invalid");
    if (!normalize_function_panic_exits(root, evidence, isolate, error, error_size))
        return false;
    for (uint16_t child = 0u; child < root->nchildren; ++child)
        if (!xi_normalize_panic_exits(root->children[child], evidence, isolate, error,
                                           error_size))
            return false;
    return true;
}
