/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregalloc.c - LIFO register management system implementation
 *
 * KEY CONCEPT:
 *   - Pure LIFO stack-based management, single freereg state
 *   - O(1) allocation and deallocation
 *   - Assert ensures LIFO order
 *   - Protection stack for consecutive argument scenarios
 */

#include "xregalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * Creation and Destruction
 * ============================================================ */

XRegAlloc *xreg_new(void) {
    XRegAlloc *ra = (XRegAlloc *) xr_malloc(sizeof(XRegAlloc));
    if (!ra)
        return NULL;
    memset(ra, 0, sizeof(XRegAlloc));
    ra->enable_debug = false;
    return ra;
}

void xreg_free(XRegAlloc *ra) {
    if (ra)
        xr_free(ra);
}

void xreg_reset(XRegAlloc *ra) {
    if (!ra)
        return;

    ra->num_locals = 0;
    ra->freereg = 0;
    ra->watermark = 0;
    ra->protect_depth = 0;
    ra->scope_depth = 0;
}

/* ============================================================
 * Register Allocation (LIFO)
 * ============================================================ */

int xreg_alloc_local(XRegAlloc *ra, const char *name) {
    (void) name;  // LIFO version doesn't store variable names
    if (!ra)
        return -1;
    XR_DCHECK(ra->freereg >= 0, "alloc_local: negative freereg");

    int reg = ra->freereg++;

    if (reg >= XREG_MAX) {
        xr_log_warning("regalloc", "register overflow (alloc_local)");
        ra->freereg--;
        return -1;
    }

    // Local variable: update num_locals
    ra->num_locals = ra->freereg;

    // Update watermark
    if (ra->freereg > ra->watermark) {
        ra->watermark = ra->freereg;
    }

    if (ra->enable_debug) {
        printf("[XReg] Alloc LOCAL: R[%d], freereg=%d, num_locals=%d\n", reg, ra->freereg,
               ra->num_locals);
    }

    return reg;
}

int xreg_alloc_temp(XRegAlloc *ra) {
    if (!ra)
        return -1;
    XR_DCHECK(ra->freereg >= 0, "alloc_temp: negative freereg");

    int reg = ra->freereg++;

    if (reg >= XREG_MAX) {
        xr_log_warning("regalloc", "register overflow (alloc_temp)");
        ra->freereg--;
        return -1;
    }

    // Update watermark
    if (ra->freereg > ra->watermark) {
        ra->watermark = ra->freereg;
    }

    if (ra->enable_debug) {
        printf("[XReg] Alloc TEMP: R[%d], freereg=%d\n", reg, ra->freereg);
    }

    return reg;
}

void xreg_free_temp(XRegAlloc *ra, int reg) {
    if (!ra)
        return;
    XR_DCHECK(reg >= 0, "free_temp: negative register");

    // Cannot free registers within local variable region
    if (reg < ra->num_locals) {
        if (ra->enable_debug) {
            printf("[XReg] Skip free LOCAL: R[%d] (num_locals=%d)\n", reg, ra->num_locals);
        }
        return;
    }

    // LIFO check: must free freereg - 1 (stack top)
    if (reg != ra->freereg - 1) {
        if (ra->enable_debug) {
            xr_log_warning("regalloc", "non-LIFO free R[%d], freereg=%d (expected %d)", reg,
                           ra->freereg, ra->freereg - 1);
        }
        // Non-LIFO free: ignore, let caller use xreg_set_freereg for batch reclaim
        return;
    }

    ra->freereg--;

    if (ra->enable_debug) {
        printf("[XReg] Free TEMP: R[%d], freereg=%d\n", reg, ra->freereg);
    }
}

int xreg_reserve(XRegAlloc *ra, int n) {
    XR_DCHECK(n > 0, "xreg_reserve: invalid n");
    if (!ra || n <= 0)
        return -1;

    int base = ra->freereg;

    if (base + n > XREG_MAX) {
        xr_log_warning("regalloc", "cannot reserve %d registers", n);
        return -1;
    }

    ra->freereg += n;

    // Update watermark
    if (ra->freereg > ra->watermark) {
        ra->watermark = ra->freereg;
    }

    if (ra->enable_debug) {
        printf("[XReg] Reserve %d: R[%d..%d], freereg=%d\n", n, base, base + n - 1, ra->freereg);
    }

    return base;
}

/* ============================================================
 * Stack Pointer Control (Core)
 * ============================================================ */

int xreg_get_freereg(XRegAlloc *ra) {
    return ra ? ra->freereg : 0;
}

void xreg_set_freereg(XRegAlloc *ra, int freereg) {
    if (!ra)
        return;
    XR_DCHECK(freereg >= 0 && freereg <= XREG_MAX, "set_freereg: out of bounds");

    // Don't allow below num_locals
    if (freereg < ra->num_locals) {
        freereg = ra->num_locals;
    }

    ra->freereg = freereg;

    // Update watermark (may be setting higher value)
    if (ra->freereg > ra->watermark) {
        ra->watermark = ra->freereg;
    }

    if (ra->enable_debug) {
        printf("[XReg] Set freereg: %d\n", freereg);
    }
}

int xreg_get_local_end(XRegAlloc *ra) {
    return ra ? ra->num_locals : 0;
}

void xreg_set_local_end(XRegAlloc *ra, int local_end) {
    if (!ra)
        return;
    ra->num_locals = local_end;

    // Ensure freereg >= num_locals
    if (ra->freereg < ra->num_locals) {
        ra->freereg = ra->num_locals;
    }

    if (ra->enable_debug) {
        printf("[XReg] Set local_end: %d\n", local_end);
    }
}

/* ============================================================
 * Protection Region Management
 * ============================================================ */

int xreg_protect_begin(XRegAlloc *ra, int base, int count, const char *reason) {
    if (!ra)
        return -1;

    if (ra->protect_depth >= XREG_PROTECT_STACK) {
        xr_log_warning("regalloc", "protect stack overflow");
        return -1;
    }

    int id = ra->protect_depth;
    XProtectRegion *region = &ra->protect_stack[id];
    region->base = base;
    region->count = count;
    region->reason = reason;

    ra->protect_depth++;

    if (ra->enable_debug) {
        printf("[XReg] Protect BEGIN: R[%d..%d] (%s), id=%d\n", base, base + count - 1,
               reason ? reason : "?", id);
    }

    return id;
}

void xreg_protect_end(XRegAlloc *ra, int protect_id) {
    if (!ra)
        return;

    // Must end in LIFO order
    XR_DCHECK(ra->protect_depth > 0, "protect_end: protect stack underflow");
    if (protect_id != ra->protect_depth - 1) {
        xr_log_warning("regalloc", "non-LIFO protect end (id=%d, depth=%d)", protect_id,
                       ra->protect_depth);
    }

    if (ra->protect_depth > 0) {
        ra->protect_depth--;
    }

    if (ra->enable_debug) {
        printf("[XReg] Protect END: id=%d, depth=%d\n", protect_id, ra->protect_depth);
    }
}

bool xreg_is_protected(XRegAlloc *ra, int reg) {
    if (!ra)
        return false;

    for (int i = 0; i < ra->protect_depth; i++) {
        XProtectRegion *region = &ra->protect_stack[i];
        if (reg >= region->base && reg < region->base + region->count) {
            return true;
        }
    }

    return false;
}

/* ============================================================
 * Scope Management
 * ============================================================ */

void xreg_scope_enter(XRegAlloc *ra) {
    if (!ra)
        return;

    if (ra->scope_depth >= XREG_SCOPE_MAX) {
        xr_log_warning("regalloc", "scope stack overflow");
        return;
    }

    // Record num_locals when entering
    ra->scope_base[ra->scope_depth] = ra->num_locals;
    ra->scope_depth++;

    if (ra->enable_debug) {
        printf("[XReg] Scope ENTER: depth=%d, num_locals=%d\n", ra->scope_depth, ra->num_locals);
    }
}

void xreg_scope_exit(XRegAlloc *ra) {
    if (!ra || ra->scope_depth <= 0)
        return;

    ra->scope_depth--;
    int base = ra->scope_base[ra->scope_depth];

    // Restore num_locals
    ra->num_locals = base;

    // Also set freereg to num_locals (release all temp registers in this scope)
    ra->freereg = ra->num_locals;

    if (ra->enable_debug) {
        printf("[XReg] Scope EXIT: depth=%d, num_locals=%d, freereg=%d\n", ra->scope_depth,
               ra->num_locals, ra->freereg);
    }
}

/* ============================================================
 * Query
 * ============================================================ */

int xreg_num_locals(XRegAlloc *ra) {
    return ra ? ra->num_locals : 0;
}

int xreg_watermark(XRegAlloc *ra) {
    return ra ? ra->watermark : 0;
}

bool xreg_is_local(XRegAlloc *ra, int reg) {
    if (!ra || reg < 0)
        return false;
    return reg < ra->num_locals;
}

/* ============================================================
 * Consecutive Allocation
 * ============================================================ */

int xreg_alloc_next(XRegAlloc *ra) {
    // Equivalent to xreg_alloc_temp
    return xreg_alloc_temp(ra);
}

void xreg_set_local(XRegAlloc *ra, int reg, const char *name) {
    (void) name;  // LIFO version doesn't store variable names

    if (!ra || reg < 0 || reg >= XREG_MAX)
        return;

    // Ensure num_locals covers this register
    if (reg >= ra->num_locals) {
        ra->num_locals = reg + 1;
    }

    // Ensure freereg >= num_locals
    if (ra->freereg < ra->num_locals) {
        ra->freereg = ra->num_locals;
    }

    // Update watermark
    if (ra->num_locals > ra->watermark) {
        ra->watermark = ra->num_locals;
    }

    if (ra->enable_debug) {
        printf("[XReg] Set LOCAL: R[%d], num_locals=%d\n", reg, ra->num_locals);
    }
}

/* ============================================================
 * Debug
 * ============================================================ */

void xreg_set_debug(XRegAlloc *ra, bool enable) {
    if (ra)
        ra->enable_debug = enable;
}

void xreg_dump(XRegAlloc *ra) {
    if (!ra) {
        printf("[XReg] Dump: NULL allocator\n");
        return;
    }

    printf("\n=== XRegAlloc State (LIFO) ===\n");
    printf("num_locals:    %d\n", ra->num_locals);
    printf("freereg:       %d\n", ra->freereg);
    printf("watermark:     %d\n", ra->watermark);
    printf("scope_depth:   %d\n", ra->scope_depth);
    printf("protect_depth: %d\n", ra->protect_depth);

    // Print protection regions
    if (ra->protect_depth > 0) {
        printf("\nProtect regions:\n");
        for (int i = 0; i < ra->protect_depth; i++) {
            XProtectRegion *region = &ra->protect_stack[i];
            printf("  [%d] R[%d..%d] (%s)\n", i, region->base, region->base + region->count - 1,
                   region->reason ? region->reason : "?");
        }
    }

    printf("==============================\n\n");
}
