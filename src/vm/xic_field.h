/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_field.h - Field access inline cache system
 *
 * KEY CONCEPT:
 *   Caches field access type and offset for monomorphic/polymorphic optimization.
 *   Monomorphic access achieves ~10x speedup, polymorphic ~7x speedup.
 */

#ifndef XIC_FIELD_H
#define XIC_FIELD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../base/xdefs.h"

typedef struct XrClass XrClass;

/* ========== IC State ========== */

typedef enum {
    XR_IC_FIELD_UNINIT = 0,  // Never accessed
    XR_IC_FIELD_MONO,        // Monomorphic (single type)
    XR_IC_FIELD_POLY,        // Polymorphic (2-4 types)
    XR_IC_FIELD_MEGA         // Megamorphic (>4 types, IC abandoned)
} XrICFieldState;

/* ========== IC Entry ========== */

// Caches access info for one type (Class/Instance only)
typedef struct XrICFieldEntry {
    XrClass *cls;
    int offset;
    uint32_t hit_count;
} XrICFieldEntry;

/* ========== Inline Cache ========== */

// Each OP_GETPROP/OP_SETPROP instruction has an associated IC
typedef struct XrICField {
    XrICFieldState state;
    uint8_t entry_count;
    int cached_symbol;         // For cache validity check
    XrICFieldEntry entries[4]; // Max 4 types cached
    uint32_t miss_count;

    // Json Shape IC: caches (shape_id, field_index) for monomorphic Json access.
    // Same OP_GETPROP either hits Json or Instance path, never both simultaneously.
    uint16_t json_shape_id;    // Cached shape id (0 = uninit)
    uint16_t json_field_idx;   // Cached field index within fields[]
#ifndef NDEBUG
    int debug_instruction_offset; // Expected instruction offset for cache validation
#endif
} XrICField;

/* ========== IC Operations ========== */

static inline void xr_ic_field_init(XrICField *ic) {
    ic->state = XR_IC_FIELD_UNINIT;
    ic->entry_count = 0;
    ic->cached_symbol = -1;
    ic->miss_count = 0;
    ic->json_shape_id = 0;
    ic->json_field_idx = 0;
#ifndef NDEBUG
    ic->debug_instruction_offset = -1;
#endif
    for (int i = 0; i < 4; i++) {
        ic->entries[i].cls = NULL;
        ic->entries[i].offset = -1;
        ic->entries[i].hit_count = 0;
    }
}

// Fast path - monomorphic lookup (Class/Instance)
// Returns true on hit, false on miss
static inline bool xr_ic_field_lookup_mono(XrICField *ic, XrClass *cls, int symbol, int *out_offset) {
    if (ic->state == XR_IC_FIELD_MONO && 
        ic->cached_symbol == symbol && 
        ic->entries[0].cls == cls) {
        *out_offset = ic->entries[0].offset;
        ic->entries[0].hit_count++;
        return true;
    }
    return false;
}

// Fast path - polymorphic lookup
// Returns true on hit, false on miss
static inline bool xr_ic_field_lookup_poly(XrICField *ic, XrClass *cls, int symbol, int *out_offset) {
    if (ic->state == XR_IC_FIELD_POLY && ic->cached_symbol == symbol) {
        for (int i = 0; i < ic->entry_count; i++) {
            if (ic->entries[i].cls == cls) {
                *out_offset = ic->entries[i].offset;
                ic->entries[i].hit_count++;
                return true;
            }
        }
    }
    return false;
}

// Json Shape IC: monomorphic lookup by shape_id
// Returns true on hit with field index in *out_idx
static inline bool xr_ic_json_lookup(XrICField *ic, uint16_t shape_id, int symbol, uint16_t *out_idx) {
    if (ic->json_shape_id == shape_id && ic->cached_symbol == symbol && shape_id != 0) {
        *out_idx = ic->json_field_idx;
        return true;
    }
    return false;
}

// Json Shape IC: update cache after miss
static inline void xr_ic_json_update(XrICField *ic, uint16_t shape_id, uint16_t field_idx, int symbol) {
    ic->json_shape_id = shape_id;
    ic->json_field_idx = field_idx;
    ic->cached_symbol = symbol;
}

// Update IC with new type info (Class/Instance)
XR_FUNC void xr_ic_field_update(XrICField *ic, XrClass *cls, int offset, int symbol);

// Reset IC - clear all cached entries
XR_FUNC void xr_ic_field_reset(XrICField *ic);

typedef struct XrICFieldStats {
    uint64_t mono_hits;
    uint64_t poly_hits;
    uint64_t misses;
    uint64_t mega_accesses;
} XrICFieldStats;

XR_FUNC void xr_ic_field_collect_stats(XrICField *ic, XrICFieldStats *stats);
XR_FUNC void xr_ic_field_print_stats(XrICFieldStats *stats);

#endif // XIC_FIELD_H
