/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_defuse.h - Def-use chains for Xi IR values
 *
 * KEY CONCEPT:
 *   For each XiValue, tracks all use sites (instructions, phis, block
 *   controls that reference this value via args[]).  Definitions are
 *   implicit in SSA: the XiValue itself is its own definition.
 *
 *   Uses are stored in a flat array with per-value offset/count indexing,
 *   enabling O(1) use-count lookup and O(n) iteration of use sites.
 *
 * WHY THIS DESIGN:
 *   - Single allocation for the entire use array (cache-friendly)
 *   - Per-value offset/count enables direct access without linked lists
 *   - Immutable after construction (rebuild if IR changes)
 *   - Supplements XiValue.uses (simple count) with detailed site info
 *
 * USAGE:
 *   XiDefUse du;
 *   xi_defuse_build(&du, func);
 *   // ... query uses ...
 *   xi_defuse_free(&du);
 */

#ifndef XI_DEFUSE_H
#define XI_DEFUSE_H

#include "xi.h"
#include "../base/xdefs.h"
#include <stdbool.h>

/* ========== Use Site ========== */

typedef enum {
    XI_USE_VALUE_ARG = 0,   /* instruction args[arg_idx] */
    XI_USE_PHI_ARG   = 1,   /* phi node value.args[arg_idx] */
    XI_USE_CONTROL   = 2,   /* block control value */
} XiUseKind;

typedef struct {
    uint32_t block_id;      /* block containing the use */
    uint32_t value_id;      /* value/phi that uses this def (UINT32_MAX for control) */
    uint8_t kind;           /* XiUseKind */
    uint8_t arg_idx;        /* argument index within the user */
} XiUseSite;

/* ========== Def-Use Chains ========== */

typedef struct XiDefUse {
    XiUseSite *sites;       /* flat array of all use records */
    uint32_t *offset;       /* offset[v] = start index in sites[] for value v */
    uint32_t *count;        /* count[v]  = number of uses for value v */
    uint32_t max_id;        /* size of offset[] and count[] arrays */
    uint32_t total_sites;   /* total entries in sites[] */
} XiDefUse;

/* ========== API ========== */

/* Build def-use chains for all values in the function.
 * Caller must call xi_defuse_free() when done. */
XR_FUNC void xi_defuse_build(XiDefUse *du, XiFunc *f);

/* Release all memory held by a XiDefUse result. */
XR_FUNC void xi_defuse_free(XiDefUse *du);

/* ========== Queries ========== */

/* Number of uses for value with given ID. */
static inline uint32_t xi_defuse_nuses(const XiDefUse *du, uint32_t vid) {
    if (!du || vid >= du->max_id) return 0;
    return du->count[vid];
}

/* Pointer to first use site for value vid (iterate count[vid] entries). */
static inline const XiUseSite *xi_defuse_uses(const XiDefUse *du, uint32_t vid) {
    if (!du || vid >= du->max_id || du->count[vid] == 0) return NULL;
    return &du->sites[du->offset[vid]];
}

/* Is value vid dead (zero uses)? */
static inline bool xi_defuse_is_dead(const XiDefUse *du, uint32_t vid) {
    return xi_defuse_nuses(du, vid) == 0;
}

/* Does value vid have exactly one use? */
static inline bool xi_defuse_single_use(const XiDefUse *du, uint32_t vid) {
    return xi_defuse_nuses(du, vid) == 1;
}

#endif // XI_DEFUSE_H
