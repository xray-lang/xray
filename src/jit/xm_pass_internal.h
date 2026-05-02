/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pass_internal.h - Internal shared declarations for Xm optimization passes
 */

#ifndef XM_PASS_INTERNAL_H
#define XM_PASS_INTERNAL_H

#include "xm_pass.h"
#include "xm_defuse.h"
#include "xm_offsets.h"
#include "../runtime/value/xchunk.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../base/xdefs.h"

// xm_op_is_pure, xm_compute_idom now declared in xm.h

/* ========== Vreg vtag helpers (shared by advanced + type passes) ========== */

// Narrow vreg ctype from UNKNOWN to concrete kind.
static inline void refine_vreg_vtag(XmFunc *func, uint32_t vi, uint8_t vtag) {
    if (vi >= func->nvreg)
        return;
    XmIns *def = func->vregs[vi].def;
    if (def && def->ctype.kind != XM_TK_UNKNOWN)
        return;
    if (!def && func->vregs[vi].rep != XR_REP_TAGGED)
        return;
    if (def)
        def->ctype.kind = vtag_to_type_kind(vtag);
}

// Force-set vreg ctype unconditionally.
static inline void force_vreg_vtag(XmFunc *func, uint32_t vi, uint8_t vtag) {
    if (vi >= func->nvreg)
        return;
    XmIns *def = func->vregs[vi].def;
    if (def)
        def->ctype.kind = vtag_to_type_kind(vtag);
}

#endif  // XM_PASS_INTERNAL_H
