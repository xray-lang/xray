/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pass_close.c - Closure metadata materialization pass
 *
 * Builds XiClosureMeta for every closure in a function tree.
 * Assigns env_offset and cell_index for each capture.
 * Determines capture_kind from escape/mutability facts.
 * Advances function stage to XI_STAGE_CLOSED.
 *
 * This pass runs after lowering and canonicalization but before
 * optimization.  All backends (VM/JIT/AOT) read the resulting
 * XiClosureMeta for env layout and capture decisions.
 */

#include "xi.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#include <string.h>

/* ========== Capture Kind Resolution ========== */

/* Determine capture_kind for a single capture based on mutability facts.
 * Called during close pass; the lowerer has already set needs_cell and
 * is_mutable from its local analysis. */
static void resolve_capture_kind(XiCapture *cap) {
    XR_DCHECK(cap != NULL, "resolve_capture_kind: NULL capture");

    if (cap->is_shared) {
        cap->capture_kind = XI_CAPTURE_CORO_SHARED;
        cap->needs_cell = true;
    } else if (cap->needs_cell || cap->is_mutable || cap->is_reassigned) {
        cap->capture_kind = XI_CAPTURE_BY_MUT_CELL;
        cap->needs_cell = true;
    } else {
        cap->capture_kind = XI_CAPTURE_BY_COPY;
        cap->needs_cell = false;
    }
}

/* ========== Env Layout Assignment ========== */

/* Assign env_offset and cell_index for all captures of a single function.
 * Returns the total env size (number of slots). */
static uint16_t assign_env_layout(XiFunc *f) {
    XR_DCHECK(f != NULL, "assign_env_layout: NULL func");

    int16_t next_offset = 0;
    int16_t next_cell = 0;

    for (uint16_t i = 0; i < f->ncaptures; i++) {
        XiCapture *cap = &f->captures[i];
        resolve_capture_kind(cap);

        cap->env_offset = next_offset++;

        if (cap->needs_cell) {
            cap->cell_index = next_cell++;
        } else {
            cap->cell_index = -1;
        }
    }

    return (uint16_t) next_offset;
}

/* ========== Build XiClosureMeta ========== */

/* Build closure metadata for a single function (if it has captures). */
static XiClosureMeta *build_closure_meta(XiFunc *f) {
    XR_DCHECK(f != NULL, "build_closure_meta: NULL func");

    if (f->ncaptures == 0)
        return NULL;

    XiClosureMeta *meta = (XiClosureMeta *) xr_calloc(1, sizeof(XiClosureMeta));
    if (!meta)
        return NULL;

    meta->function = f;
    meta->parent_func = NULL; /* set by caller who knows parent */
    meta->captures = f->captures;
    meta->ncaptures = f->ncaptures;

    /* Assign offsets and cell indices */
    meta->env_size = assign_env_layout(f);

    /* Count cells and check for mutable captures */
    meta->ncells = 0;
    meta->has_mutable_capture = false;
    for (uint16_t i = 0; i < f->ncaptures; i++) {
        if (f->captures[i].needs_cell) {
            meta->ncells++;
            meta->has_mutable_capture = true;
        }
    }

    /* Direct-callable if no mutable captures and no captures at all
     * that require a closure env allocation.  For now, conservative:
     * only truly capturable closures with all by-copy captures and
     * zero cells can be direct-called. */
    meta->is_direct_callable = (meta->ncells == 0);

    f->closure_meta = meta;
    return meta;
}

/* ========== Recursive Tree Walk ========== */

/* Process a function and all its children recursively.
 * Builds XiClosureMeta for every closure, assigns env layout,
 * and advances stage to XI_STAGE_CLOSED. */
static void close_func_recursive(XiFunc *f, XiFunc *parent) {
    XR_DCHECK(f != NULL, "close_func_recursive: NULL func");

    /* Skip if already closed */
    if (f->stage >= XI_STAGE_CLOSED)
        return;

    /* Process children first (bottom-up: leaf closures before parents) */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            close_func_recursive(f->children[i], f);
    }

    /* Build closure meta if this function captures variables */
    if (f->ncaptures > 0) {
        XiClosureMeta *meta = build_closure_meta(f);
        if (meta) {
            meta->parent_func = parent;
        }
    }

    /* Advance stage */
    if (f->stage < XI_STAGE_CLOSED) {
        f->stage = XI_STAGE_CLOSED;
        f->invariant_mask |= XI_INV_UPVALS_RESOLVED;
    }
}

/* ========== Module-Level Cell Index Assignment ========== */

/* Backfill cell_index into XiModuleExport for live bindings.
 * Live bindings (mutable exports) need cell indirection so that
 * importers see updated values.  For now, assign cell indices
 * sequentially for any export marked as is_live_binding. */
static void assign_export_cell_indices(XiModule *mod) {
    if (!mod || !mod->exports)
        return;

    int16_t next_cell = 0;
    for (uint16_t i = 0; i < mod->nexports; i++) {
        XiModuleExport *exp = &mod->exports[i];
        if (exp->is_live_binding) {
            exp->cell_index = next_cell++;
        }
        /* Non-live exports keep cell_index = -1 (set during lowering) */
    }
}

/* Collect all XiClosureMeta pointers into the module's array. */
static void collect_closure_metas(XiModule *mod, XiFunc *f) {
    XR_DCHECK(mod != NULL, "collect_closure_metas: NULL module");
    XR_DCHECK(f != NULL, "collect_closure_metas: NULL func");

    if (f->closure_meta) {
        /* Grow array */
        uint16_t new_count = mod->nclosure_metas + 1;
        XiClosureMeta **new_arr = (XiClosureMeta **) xr_calloc(new_count, sizeof(XiClosureMeta *));
        if (new_arr) {
            if (mod->closure_metas) {
                memcpy(new_arr, mod->closure_metas, mod->nclosure_metas * sizeof(XiClosureMeta *));
                xr_free(mod->closure_metas);
            }
            new_arr[mod->nclosure_metas] = f->closure_meta;
            mod->closure_metas = new_arr;
            mod->nclosure_metas = new_count;
        }
    }

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            collect_closure_metas(mod, f->children[i]);
    }
}

/* ========== Public API ========== */

XR_FUNC void xi_pass_close(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_pass_close: NULL func");

    /* Walk the entire function tree bottom-up */
    close_func_recursive(f, NULL);

    /* If this function has a module, do module-level work */
    XiModule *mod = f->module;
    if (mod) {
        assign_export_cell_indices(mod);
        collect_closure_metas(mod, f);
    }
}
