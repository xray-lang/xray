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
 * Stage publication is owned by xi_program_close().
 *
 * This pass runs after lowering and canonicalization but before
 * optimization.  All backends (VM/AOT) read the resulting
 * XiClosureMeta for env layout and capture decisions.
 */

#include "xi.h"
#include "xi_effect.h"
#include "xi_module.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"

#include <string.h>

/* ========== Capture Kind Resolution ========== */

/* Determine capture_kind for a single capture based on mutability facts.
 * Called during close pass; the lowerer has already set needs_cell and
 * is_mutable from its local analysis. */
static void resolve_capture_kind(XiCapture *cap) {
    XR_DCHECK(cap != NULL, "resolve_capture_kind: NULL capture");

    if (cap->capture_kind == XI_CAPTURE_SHARED) {
        cap->capture_kind = XI_CAPTURE_SHARED;
        cap->needs_cell = true;
    } else if (cap->is_mutable || cap->is_reassigned) {
        cap->capture_kind = XI_CAPTURE_BY_MUT_CELL;
        cap->needs_cell = true;
    } else {
        cap->capture_kind = XI_CAPTURE_BY_COPY;
        /* Preserve a representation-only cell introduced for a hoisted
         * read-only capture.  It remains BY_COPY across execution. */
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

/* Process a function and all its children recursively. */
static void close_func_recursive(XiFunc *f, XiFunc *parent) {
    XR_DCHECK(f != NULL, "close_func_recursive: NULL func");

    /* Process children first (bottom-up: leaf closures before parents) */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            close_func_recursive(f->children[i], f);
    }

    /* Build closure meta if this function captures variables */
    if (f->ncaptures > 0 && !f->closure_meta) {
        XiClosureMeta *meta = build_closure_meta(f);
        if (meta) {
            meta->parent_func = parent;
        }
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

/* ========== First-Class Mutable Capture Cells ========== */

typedef struct XiLocalCellBinding {
    XiVarId var_id;
    XiValue *seed;
    XiValue *cell;
} XiLocalCellBinding;

static XiLocalCellBinding *find_local_cell_binding(XiLocalCellBinding *bindings, uint32_t count,
                                                   const XiValue *value) {
    if (!bindings || !value)
        return NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (xi_var_id_is_valid(value->var_id) && bindings[i].var_id == value->var_id)
            return &bindings[i];
        if (!xi_var_id_is_valid(value->var_id) && bindings[i].seed == value)
            return &bindings[i];
    }
    return NULL;
}

static bool value_is_existing_cell_ref(const XiFunc *f, const XiValue *value) {
    return value &&
           (value->op == XI_CELL_NEW ||
            (value->op == XI_LOAD_UPVAL && value->aux_int >= 0 && value->aux_int < f->ncaptures &&
             f->captures[value->aux_int].needs_cell));
}

static XiLocalCellBinding *collect_local_cell_bindings(XiFunc *f, uint32_t *out_count) {
    uint32_t capacity = 0;
    for (uint16_t fi = 0; fi < f->nchildren; fi++) {
        const XiFunc *child = f->children[fi];
        if (!child)
            continue;
        for (uint16_t ci = 0; ci < child->ncaptures; ci++)
            capacity += child->captures[ci].source == XI_CAPTURE_SRC_REG &&
                        child->captures[ci].needs_cell && child->captures[ci].value;
    }
    *out_count = 0;
    if (capacity == 0)
        return NULL;
    XiLocalCellBinding *bindings = (XiLocalCellBinding *) xi_func_arena_alloc(
        f, capacity * (uint32_t) sizeof(XiLocalCellBinding));
    XR_CHECK(bindings != NULL, "xi_pass_close: out of memory allocating capture cell bindings");

    for (uint16_t fi = 0; fi < f->nchildren; fi++) {
        XiFunc *child = f->children[fi];
        if (!child)
            continue;
        for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
            XiCapture *cap = &child->captures[ci];
            if (cap->source != XI_CAPTURE_SRC_REG || !cap->needs_cell || !cap->value ||
                find_local_cell_binding(bindings, *out_count, cap->value))
                continue;
            XiLocalCellBinding *binding = &bindings[(*out_count)++];
            binding->var_id = cap->value->var_id;
            binding->seed = cap->value;
            binding->cell = value_is_existing_cell_ref(f, cap->value) ? cap->value : NULL;
        }
    }
    return bindings;
}

static void materialize_local_cells(XiFunc *f, XiLocalCellBinding *bindings,
                                    uint32_t binding_count) {
    if (!bindings || binding_count == 0 || !f->entry)
        return;
    bool needs_new_cell = false;
    for (uint32_t i = 0; i < binding_count; i++)
        needs_new_cell |= bindings[i].cell == NULL;
    if (!needs_new_cell)
        return;

    XiValue *first = f->entry->nvalues > 0 ? f->entry->values[0] : NULL;
    struct XrType *null_type = xr_type_new_null(NULL);
    struct XrType *cell_type = xr_type_new_unknown(NULL);
    struct XrType *unit_type = xr_type_new_unit(NULL);
    XR_CHECK(null_type && cell_type && unit_type,
             "xi_pass_close: out of memory allocating cell IR types");
    XiValue *null_value = first ? xi_value_insert_before(f, f->entry, first, XI_CONST, null_type, 0)
                                : xi_value_new(f, f->entry, XI_CONST, null_type, 0);
    XR_CHECK(null_value != NULL, "xi_pass_close: out of memory allocating cell initializer");
    XiValue *anchor = null_value;

    for (uint32_t i = 0; i < binding_count; i++) {
        if (bindings[i].cell)
            continue;
        XiValue *cell = xi_value_insert_after(f, f->entry, anchor, XI_CELL_NEW, cell_type, 1);
        XR_CHECK(cell != NULL, "xi_pass_close: out of memory materializing capture cell");
        cell->args[0] = null_value;
        bindings[i].cell = cell;
        anchor = cell;
    }

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op == XI_CELL_NEW || v->op == XI_CELL_GET || v->op == XI_CELL_SET)
                continue;
            XiLocalCellBinding *binding =
                find_local_cell_binding(bindings, binding_count, v);
            if (!binding || !binding->cell || binding->cell == v)
                continue;
            XiValue *set = xi_value_insert_after(f, blk, v, XI_CELL_SET, unit_type, 2);
            XR_CHECK(set != NULL, "xi_pass_close: out of memory materializing cell write");
            set->args[0] = binding->cell;
            set->args[1] = v;
            set->line = v->line;
            vi++;
        }
    }
}

static void rewrite_cell_uses(XiFunc *f, XiLocalCellBinding *bindings, uint32_t binding_count) {
    struct XrType *cell_type = xr_type_new_unknown(NULL);
    XR_CHECK(cell_type != NULL, "xi_pass_close: out of memory allocating cell reference type");
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;

            XiLocalCellBinding *read_binding =
                xi_copy_is_cell_read(v) && v->nargs == 1
                    ? find_local_cell_binding(bindings, binding_count, v->args[0])
                    : NULL;
            if (read_binding && read_binding->cell) {
                v->op = XI_CELL_GET;
                v->args[0] = read_binding->cell;
                v->aux_int = 0;
                v->flags = xi_op_default_effects(v->op);
                continue;
            }

            if (v->op == XI_CLOSURE_NEW && v->aux) {
                XiFunc *child = (XiFunc *) v->aux;
                XR_CHECK(v->nargs == child->ncaptures,
                         "xi_pass_close: closure capture argument count mismatch");
                uint16_t inserted = 0;
                for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                    XiCapture *cap = &child->captures[ci];
                    if (!cap->needs_cell)
                        continue;
                    if (cap->source == XI_CAPTURE_SRC_REG) {
                        XiLocalCellBinding *binding =
                            find_local_cell_binding(bindings, binding_count, cap->value);
                        XR_CHECK(binding && binding->cell,
                                 "xi_pass_close: mutable local capture has no cell value");
                        v->args[ci] = binding->cell;
                        cap->value = v->args[ci];
                        continue;
                    }

                    XR_CHECK(cap->source == XI_CAPTURE_SRC_UPVAL && cap->index < f->ncaptures &&
                                 f->captures[cap->index].needs_cell,
                             "xi_pass_close: mutable transitive capture has no cell upvalue");
                    XiValue *raw =
                        xi_value_insert_before(f, blk, v, XI_LOAD_UPVAL, cell_type, 0);
                    XR_CHECK(raw != NULL,
                             "xi_pass_close: out of memory forwarding transitive capture cell");
                    raw->aux_int = cap->index;
                    raw->line = v->line;
                    v->args[ci] = raw;
                    cap->source = XI_CAPTURE_SRC_REG;
                    cap->index = 0;
                    cap->value = raw;
                    inserted++;
                }
                vi += inserted;
                continue;
            }

            if (v->op == XI_LOAD_UPVAL && v->aux_int >= 0 && v->aux_int < f->ncaptures &&
                f->captures[v->aux_int].needs_cell) {
                XiValue *raw = xi_value_insert_before(f, blk, v, XI_LOAD_UPVAL, cell_type, 0);
                XR_CHECK(raw != NULL, "xi_pass_close: out of memory materializing cell read");
                raw->aux_int = v->aux_int;
                raw->line = v->line;
                v->op = XI_CELL_GET;
                v->nargs = 1;
                v->args = (XiValue **) xi_func_arena_alloc(f, sizeof(XiValue *));
                XR_CHECK(v->args != NULL, "xi_pass_close: out of memory allocating cell read args");
                v->args[0] = raw;
                v->aux_int = 0;
                v->flags = xi_op_default_effects(v->op);
                vi++;
                continue;
            }

            if (v->op == XI_STORE_UPVAL && v->aux_int >= 0 && v->aux_int < f->ncaptures &&
                f->captures[v->aux_int].needs_cell && v->nargs == 1) {
                XiValue *stored = v->args[0];
                XiValue *raw = xi_value_insert_before(f, blk, v, XI_LOAD_UPVAL, cell_type, 0);
                XR_CHECK(raw != NULL, "xi_pass_close: out of memory materializing cell write");
                raw->aux_int = v->aux_int;
                raw->line = v->line;
                v->op = XI_CELL_SET;
                v->nargs = 2;
                v->args = (XiValue **) xi_func_arena_alloc(f, 2u * sizeof(XiValue *));
                XR_CHECK(v->args != NULL, "xi_pass_close: out of memory allocating cell write args");
                v->args[0] = raw;
                v->args[1] = stored;
                v->aux_int = 0;
                v->flags = xi_op_default_effects(v->op);
                vi++;
            }
        }
    }
}

static void materialize_cells_recursive(XiFunc *f) {
    uint32_t binding_count = 0;
    XiLocalCellBinding *bindings = collect_local_cell_bindings(f, &binding_count);
    materialize_local_cells(f, bindings, binding_count);
    rewrite_cell_uses(f, bindings, binding_count);
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            materialize_cells_recursive(f->children[i]);
    }
}

/* ========== Public API ========== */

XR_FUNC void xi_pass_close(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_pass_close: NULL func");

    /* Walk the entire function tree bottom-up */
    close_func_recursive(f, NULL);
    materialize_cells_recursive(f);

    /* If this function has a module, do module-level work */
    XiModule *mod = f->module;
    if (mod) {
        assign_export_cell_indices(mod);
        collect_closure_metas(mod, f);
    }
}
