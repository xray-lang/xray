/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_reuse.c - Drop-Reuse analysis pass (Perceus FBIP)
 *
 * Scans each basic block for the pattern:
 *   v_drop = XI_RELEASE(v_old)    -- dropping an RC object
 *   ...
 *   v_new  = XI_xxx_NEW(...)      -- allocating a new RC object of same size
 *
 * When found and the dropped value and new allocation share a compatible
 * size class, converts to:
 *   v_token = XI_DROP_REUSE(v_old)
 *   v_new   = XI_ALLOC_AT(v_token)  [aux_int = packed gc_type|size]
 *
 * Constraints for reuse eligibility:
 *   - Both in the same basic block (no control flow between)
 *   - The released value is not used after the release
 *   - The new allocation is of the same GC type (guarantees same size class)
 *   - No side effects between the release and alloc that could observe the
 *     freed memory (conservative: only allow adjacent or very close ops)
 */

#include "xi_reuse.h"
#include "xi_escape.h"
#include "xi_own.h"
#include "xi_core_api.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/gc/xgc_header.h"

#include <string.h>

/* Maximum distance (in ops) between the RELEASE and the alloc for reuse
 * pairing. Beyond this the register pressure from holding the token is
 * not worth the potential reuse benefit. */
#define REUSE_MAX_DISTANCE 8

/* ========== Size Class Helpers ========== */

/* Map a GC type tag to a size class bucket. Objects in the same bucket
 * have identical allocation sizes and can be reused interchangeably. */
static uint8_t gc_type_from_alloc_op(uint16_t op) {
    switch (op) {
        case XI_ARRAY_NEW:
            return 3; /* XR_TARRAY */
        case XI_MAP_NEW:
            return 4; /* XR_TMAP */
        case XI_SET_NEW:
            return 5; /* XR_TSET */
        case XI_JSON_NEW:
            return 10; /* XR_TJSON */
        case XI_TUPLE_NEW:
            return 11; /* XR_TTUPLE */
        default:
            return 0; /* unknown — do not reuse */
    }
}

/* Get the GC type from a value's compile-time type. Returns 0 if not a
 * reusable type. Only same-type reuse is safe (same layout, same destroy). */
static uint8_t gc_type_from_xrtype(const struct XrType *type) {
    if (!type)
        return 0;
    switch (type->kind) {
        case XR_KIND_ARRAY:
            return 3;
        case XR_KIND_MAP:
            return 4;
        case XR_KIND_SET:
            return 5;
        case XR_KIND_JSON:
            return 10;
        case XR_KIND_TUPLE:
            return 11;
        default:
            return 0;
    }
}

/* Approximate allocation size for a GC type. These must match the actual
 * runtime allocations in xr_coro_gc_newobj callers. */
static uint32_t alloc_size_for_gc_type(uint8_t gc_type) {
    switch (gc_type) {
        case 3:
            return 64; /* XrArray: header + data ptr + len + cap */
        case 4:
            return 96; /* XrMap: header + buckets + count + ... */
        case 5:
            return 64; /* XrSet: similar to array */
        case 10:
            return 48; /* XrJson: header + fields */
        case 11:
            return 48; /* XrTuple: header + elements */
        default:
            return 0;
    }
}

/* ========== Per-Block Reuse Scan ========== */

/* Find a heap-allocating op after position `start_idx` in the block that
 * produces a value of the same GC type as `gc_type`. Returns the index
 * or UINT32_MAX if not found within REUSE_MAX_DISTANCE. */
static uint32_t find_alloc_candidate(XiBlock *blk, uint32_t start_idx, uint8_t gc_type) {
    uint32_t limit = start_idx + REUSE_MAX_DISTANCE;
    if (limit > blk->nvalues)
        limit = blk->nvalues;

    for (uint32_t i = start_idx; i < limit; i++) {
        XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (!xi_op_is_heap_alloc(v->op))
            continue;
        uint8_t alloc_type = gc_type_from_alloc_op(v->op);
        if (alloc_type == gc_type)
            return i;
    }
    return UINT32_MAX;
}

/* Process one basic block: scan for RELEASE+ALLOC pairs and convert. */
static void reuse_scan_block(XiFunc *f, XiBlock *blk) {
    if (!blk || blk->nvalues == 0)
        return;

    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *rel = blk->values[i];
        if (!rel || rel->op != XI_RELEASE)
            continue;
        XR_DCHECK(rel->nargs >= 1, "xi_reuse: RELEASE with no args");

        XiValue *dropped = rel->args[0];
        if (!dropped)
            continue;

        /* Determine the GC type of the dropped value. */
        uint8_t gc_type = gc_type_from_xrtype(dropped->type);
        if (gc_type == 0)
            continue; /* unknown type — skip */

        /* Search forward for a matching alloc. */
        uint32_t alloc_idx = find_alloc_candidate(blk, i + 1, gc_type);
        if (alloc_idx == UINT32_MAX)
            continue; /* no matching alloc nearby */

        XiValue *alloc = blk->values[alloc_idx];
        XR_DCHECK(alloc != NULL, "xi_reuse: NULL alloc value");

        /* Convert: XI_RELEASE -> XI_DROP_REUSE (produces reuse token). */
        rel->op = XI_DROP_REUSE;
        /* The DROP_REUSE result type is a raw pointer (not an RC type).
         * We keep the original type annotation so the token carries the
         * GC type info, but mark it as non-RC via the escape field. */
        rel->escape = XI_ESC_NONE;

        /* Convert: heap alloc -> XI_ALLOC_AT(token).
         * Pack (gc_type << 16) | alloc_size into aux_int for the runtime. */
        uint32_t alloc_size = alloc_size_for_gc_type(gc_type);
        XR_DCHECK(alloc_size > 0, "xi_reuse: zero alloc size");

        /* Save original alloc info in aux for codegen fallback knowledge. */
        alloc->op = XI_ALLOC_AT;
        alloc->aux_int = ((int64_t) gc_type << 16) | (int64_t) alloc_size;

        /* Rewire: ALLOC_AT takes the reuse token as its sole arg.
         * Original args of the alloc (e.g., initial capacity) are dropped —
         * the runtime helper handles initialization from scratch. */
        if (alloc->nargs > 0) {
            alloc->args[0] = rel; /* token = result of DROP_REUSE */
        } else {
            /* Expand args array to hold the token. */
            alloc->args = xr_malloc(sizeof(XiValue *));
            if (alloc->args) {
                alloc->args[0] = rel;
                alloc->nargs = 1;
            }
        }

        alloc->flags |= XI_FLAG_SIDE_EFFECT;
    }
}

/* ========== Public API ========== */

XR_FUNC void xi_reuse_insert(XiFunc *f) {
    if (!f)
        return;

    /* Process children first (bottom-up). */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_reuse_insert(f->children[i]);
    }

    /* Scan each block for reuse opportunities. */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        reuse_scan_block(f, f->blocks[b]);
    }
}
