/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass_internal.h - Internal shared declarations for XIR optimization passes
 */

#ifndef XIR_PASS_INTERNAL_H
#define XIR_PASS_INTERNAL_H

#include "xir_pass.h"
#include "xir_defuse.h"
#include "xir_builder.h"
#include "xir_offsets.h"
#include "../runtime/value/xchunk.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../base/xdefs.h"

// xir_op_is_pure, xir_compute_idom now declared in xir.h

/* ========== Vreg vtag helpers (shared by advanced + type passes) ========== */

// Narrow vreg ctype from UNKNOWN to concrete kind.
static inline void refine_vreg_vtag(XirFunc *func, uint32_t vi, uint8_t vtag) {
    if (vi >= func->nvreg)
        return;
    XirIns *def = func->vregs[vi].def;
    if (def && def->ctype.kind != XIR_TK_UNKNOWN)
        return;
    if (!def && func->vregs[vi].rep != XR_REP_TAGGED)
        return;
    if (def)
        def->ctype.kind = vtag_to_type_kind(vtag);
}

// Force-set vreg ctype unconditionally.
static inline void force_vreg_vtag(XirFunc *func, uint32_t vi, uint8_t vtag) {
    if (vi >= func->nvreg)
        return;
    XirIns *def = func->vregs[vi].def;
    if (def)
        def->ctype.kind = vtag_to_type_kind(vtag);
}

#endif  // XIR_PASS_INTERNAL_H
