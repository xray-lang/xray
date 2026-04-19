/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_alias.c - Minimal alias-provenance classifier.
 *
 * The cache is sized to func->nvreg; unreachable / unused slots stay
 * as the (UNKNOWN, origin=0) default returned by xr_calloc.  The
 * analysis is intentionally cheap — it walks each vreg's defining
 * instruction once and traces MOV/COPY chains backwards to resolve
 * the ultimate origin.
 */

#include "xir_alias.h"

#include "xir.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#define XIR_REF_VREG_MASK  0x80000000u
#define XIR_REF_INDEX_MASK 0x0FFFFFFFu

static inline bool ref_is_vreg(XirRef r)  { return (r & XIR_REF_VREG_MASK) != 0; }
static inline uint32_t ref_index(XirRef r) { return r & XIR_REF_INDEX_MASK; }

typedef struct {
    XirAliasInfo *infos;   // [nvreg]
    uint32_t      nvreg;
} AliasTable;

/* Classify a vreg by walking its defining instruction. */
static XirAliasInfo classify(XirFunc *func, uint32_t v) {
    XirAliasInfo out = { XIR_ALIAS_UNKNOWN, 0 };
    if (v >= func->nvreg) return out;

    XirIns *def = func->vregs[v].def;
    if (!def) {
        /* No defining instruction → incoming parameter slot. */
        out.source = XIR_ALIAS_PARAM;
        out.origin = (XIR_REF_VREG_MASK | v);
        return out;
    }

    /* Trace through trivial copies / MOV chains so v2 = MOV v1 inherits
     * v1's provenance.  Guard against cycles with a generous hop
     * count — well-formed SSA never chains beyond a handful of MOVs. */
    uint32_t cur = v;
    for (int hop = 0; hop < 32; hop++) {
        if (cur >= func->nvreg) break;
        XirIns *d = func->vregs[cur].def;
        if (!d) {
            out.source = XIR_ALIAS_PARAM;
            out.origin = (XIR_REF_VREG_MASK | cur);
            return out;
        }
        if (d->op == XIR_ALLOC) {
            out.source = XIR_ALIAS_FRESH_ALLOC;
            out.origin = d->dst;
            return out;
        }
        if (xir_op_is_copy(d->op) && ref_is_vreg(d->args[0])) {
            cur = ref_index(d->args[0]);
            continue;
        }
        break;
    }
    return out;
}

const XirAliasInfo *xir_func_get_alias(XirFunc *func, XirRef vreg_ref) {
    if (!func || !ref_is_vreg(vreg_ref)) return NULL;
    uint32_t v = ref_index(vreg_ref);
    if (v >= func->nvreg) return NULL;

    AliasTable *tab = (AliasTable *)func->alias;
    if (!tab) {
        tab = (AliasTable *)xr_calloc(1, sizeof(AliasTable));
        if (!tab) return NULL;
        tab->nvreg = func->nvreg;
        tab->infos = (XirAliasInfo *)xr_calloc(tab->nvreg,
                                                 sizeof(XirAliasInfo));
        if (!tab->infos) { xr_free(tab); return NULL; }
        func->alias = tab;
    }

    XirAliasInfo *slot = &tab->infos[v];
    /* Zero-initialised entries have source=UNKNOWN, origin=0 — matches
     * the "not yet classified" sentinel as well. */
    if (slot->source == XIR_ALIAS_UNKNOWN && slot->origin == 0) {
        *slot = classify(func, v);
    }
    return slot;
}

void xir_func_invalidate_alias(XirFunc *func) {
    if (!func || !func->alias) return;
    AliasTable *tab = (AliasTable *)func->alias;
    xr_free(tab->infos);
    xr_free(tab);
    func->alias = NULL;
}
