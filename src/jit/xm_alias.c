/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_alias.c - Minimal alias-provenance classifier.
 *
 * The cache is sized to func->nvreg; unreachable / unused slots stay
 * as the (UNKNOWN, origin=0) default returned by xr_calloc.  The
 * analysis is intentionally cheap — it walks each vreg's defining
 * instruction once and traces MOV/COPY chains backwards to resolve
 * the ultimate origin.
 */

#include "xm_alias.h"

#include "xm.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#define XM_REF_VREG_MASK 0x80000000u
#define XM_REF_INDEX_MASK 0x0FFFFFFFu

static inline bool ref_is_vreg(XmRef r) {
    return (r & XM_REF_VREG_MASK) != 0;
}
static inline uint32_t ref_index(XmRef r) {
    return r & XM_REF_INDEX_MASK;
}

typedef struct {
    XmAliasInfo *infos;  // [nvreg]
    uint32_t nvreg;
} AliasTable;

/* Classify a vreg by walking its defining instruction. */
static XmAliasInfo classify(XmFunc *func, uint32_t v) {
    XmAliasInfo out = {XM_ALIAS_UNKNOWN, 0};
    if (v >= func->nvreg)
        return out;

    XmIns *def = func->vregs[v].def;
    if (!def) {
        /* No defining instruction → incoming parameter slot. */
        out.source = XM_ALIAS_PARAM;
        out.origin = (XM_REF_VREG_MASK | v);
        return out;
    }

    /* Trace through trivial copies / MOV chains so v2 = MOV v1 inherits
     * v1's provenance.  Guard against cycles with a generous hop
     * count — well-formed SSA never chains beyond a handful of MOVs. */
    uint32_t cur = v;
    for (int hop = 0; hop < 32; hop++) {
        if (cur >= func->nvreg)
            break;
        XmIns *d = func->vregs[cur].def;
        if (!d) {
            out.source = XM_ALIAS_PARAM;
            out.origin = (XM_REF_VREG_MASK | cur);
            return out;
        }
        if (d->op == XM_ALLOC) {
            out.source = XM_ALIAS_FRESH_ALLOC;
            out.origin = d->dst;
            return out;
        }
        if (xm_op_is_copy(d->op) && ref_is_vreg(d->args[0])) {
            cur = ref_index(d->args[0]);
            continue;
        }
        break;
    }
    return out;
}

const XmAliasInfo *xm_func_get_alias(XmFunc *func, XmRef vreg_ref) {
    if (!func || !ref_is_vreg(vreg_ref))
        return NULL;
    uint32_t v = ref_index(vreg_ref);
    if (v >= func->nvreg)
        return NULL;

    AliasTable *tab = (AliasTable *) func->alias;
    if (!tab) {
        tab = (AliasTable *) xr_calloc(1, sizeof(AliasTable));
        if (!tab)
            return NULL;
        tab->nvreg = func->nvreg;
        tab->infos = (XmAliasInfo *) xr_calloc(tab->nvreg, sizeof(XmAliasInfo));
        if (!tab->infos) {
            xr_free(tab);
            return NULL;
        }
        func->alias = tab;
    }

    XmAliasInfo *slot = &tab->infos[v];
    /* Zero-initialised entries have source=UNKNOWN, origin=0 — matches
     * the "not yet classified" sentinel as well. */
    if (slot->source == XM_ALIAS_UNKNOWN && slot->origin == 0) {
        *slot = classify(func, v);
    }
    return slot;
}

void xm_func_invalidate_alias(XmFunc *func) {
    if (!func || !func->alias)
        return;
    AliasTable *tab = (AliasTable *) func->alias;
    xr_free(tab->infos);
    xr_free(tab);
    func->alias = NULL;
}
