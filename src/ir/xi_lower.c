/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower.c - AST to typed SSA IR lowering (Braun SSA construction)
 *
 * Single-pass recursive walk over the AST, producing XiFunc with
 * on-the-fly SSA construction via the Braun algorithm.
 */

#include "xi_lower.h"
#include "xi_lower_internal.h"
#include "xi.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xconsteval.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"
#include "../frontend/lexer/xlex.h"
#include "../analysis/xglobal_summary.h"

#include "../runtime/class/xenum.h"
#include "../runtime/object/xstring.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/xisolate_api.h"
#include "../api/xrepl.h"
#include "../toolchain/xcompiler_session.h"

#include <string.h>
#include <stdio.h>

/* Forward declarations */
static void finalize_capture_metadata(XiFunc *f);

static bool xi_lower_is_builtin_call(const XiValue *v, const char *name) {
    return v && v->op == XI_CALL_BUILTIN && v->aux && name &&
           strcmp((const char *) v->aux, name) == 0;
}

static XiValue *xi_lower_wrap_shared_store_copy(XiLower *l, XiValue *val) {
    if (!l || !val || !xi_lower_is_builtin_call(val, "copy"))
        return val;
    val->aux = (void *) "copy_shared";
    return val;
}

XR_FUNC bool xi_lower_reject_error_type(XiLower *l, const struct XrType *type, const char *context,
                                        int line) {
    if (!xr_type_contains_error(type))
        return false;
    if (l)
        l->had_error = true;
    fprintf(stderr, "[LOWER] ErrorType cannot enter executable lowering");
    if (context && context[0])
        fprintf(stderr, " for %s", context);
    if (line > 0)
        fprintf(stderr, " at line %d", line);
    fprintf(stderr, "\n");
    return true;
}

XR_FUNC struct XrType *xi_lower_type_or_any(XiLower *l, struct XrType *type, const char *context,
                                            int line) {
    if (xi_lower_reject_error_type(l, type, context, line))
        return l ? l->type_any : NULL;
    return type ? type : (l ? l->type_any : NULL);
}

static struct XrType *xi_lower_param_type(XiLower *l, XrParamNode *param) {
    if (l && l->analyzer && param && param->symbol_id != 0) {
        XaSymbol *sym = xa_scope_lookup_by_id(l->analyzer->global_scope, param->symbol_id);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(l->analyzer, sym) : NULL;
        if (links && links->type && xr_type_contains_error(links->type))
            return links->type;
        if (links && links->type && !XR_TYPE_IS_UNKNOWN(links->type))
            return links->type;
    }
    struct XrType *type =
        (l && l->analyzer && param && param->type)
            ? xr_tref_resolve_in_analyzer(l->analyzer, param->type)
            : ((param && param->type) ? xr_tref_resolve(l ? l->isolate : NULL, param->type) : NULL);
    return type ? type : (l ? l->type_any : NULL);
}

/* ========== Dynamic capacity growth (vars / blocks) ========== */

/* Grow the variable dimension: vars, the parallel shared-slot tables, and the
 * row dimension of var_defs.  var_defs is row-major (var_id*block_cap+block_id),
 * so adding rows preserves existing entries — a plain realloc + zero-new-rows,
 * no re-layout.  New shared_map entries are seeded to -1 (not shared). */
static void xi_lower_grow_vars(XiLower *l, int need) {
    if (need <= l->var_cap)
        return;
    int nc = l->var_cap > 0 ? l->var_cap : XI_LOWER_INIT_VARS;
    while (nc < need)
        nc *= 2;

    l->vars = (XiVarEntry *) xr_realloc(l->vars, (size_t) nc * sizeof(XiVarEntry));
    l->shared_map = (int16_t *) xr_realloc(l->shared_map, (size_t) nc * sizeof(int16_t));
    l->shared_slot_funcs =
        (struct XiFunc **) xr_realloc(l->shared_slot_funcs, (size_t) nc * sizeof(struct XiFunc *));
    l->shared_slot_classes = (struct XiClassData **) xr_realloc(
        l->shared_slot_classes, (size_t) nc * sizeof(struct XiClassData *));
    l->shared_slot_enums =
        (XiEnumData **) xr_realloc(l->shared_slot_enums, (size_t) nc * sizeof(XiEnumData *));
    l->shared_slot_imports =
        (XiImportRef **) xr_realloc(l->shared_slot_imports, (size_t) nc * sizeof(XiImportRef *));
    l->var_defs = (XiValue **) xr_realloc(l->var_defs,
                                          (size_t) nc * (size_t) l->block_cap * sizeof(XiValue *));
    XR_CHECK(l->vars && l->shared_map && l->shared_slot_funcs && l->shared_slot_classes &&
                 l->shared_slot_enums && l->shared_slot_imports && l->var_defs,
             "xi_lower: grow vars OOM");

    int added = nc - l->var_cap;
    memset(&l->vars[l->var_cap], 0, (size_t) added * sizeof(XiVarEntry));
    for (int i = l->var_cap; i < nc; i++)
        l->shared_map[i] = -1;
    memset(&l->shared_slot_funcs[l->var_cap], 0, (size_t) added * sizeof(struct XiFunc *));
    memset(&l->shared_slot_classes[l->var_cap], 0, (size_t) added * sizeof(struct XiClassData *));
    memset(&l->shared_slot_enums[l->var_cap], 0, (size_t) added * sizeof(XiEnumData *));
    memset(&l->shared_slot_imports[l->var_cap], 0, (size_t) added * sizeof(XiImportRef *));
    memset(&l->var_defs[(size_t) l->var_cap * (size_t) l->block_cap], 0,
           (size_t) added * (size_t) l->block_cap * sizeof(XiValue *));
    l->var_cap = nc;
}

/* Grow the block dimension (stride) of var_defs.  Changing the stride moves
 * every row, so this reallocates and re-lays-out the existing entries. */
static void xi_lower_grow_blocks(XiLower *l, int need) {
    if (need <= l->block_cap)
        return;
    int nbc = l->block_cap > 0 ? l->block_cap : XI_LOWER_INIT_BLOCKS;
    while (nbc < need)
        nbc *= 2;

    XiValue **nd = (XiValue **) xr_calloc((size_t) l->var_cap * (size_t) nbc, sizeof(XiValue *));
    XR_CHECK(nd != NULL, "xi_lower: grow blocks OOM");
    for (int v = 0; v < l->var_cap; v++) {
        memcpy(&nd[(size_t) v * (size_t) nbc], &l->var_defs[(size_t) v * (size_t) l->block_cap],
               (size_t) l->block_cap * sizeof(XiValue *));
    }
    xr_free(l->var_defs);
    l->var_defs = nd;
    l->block_cap = nbc;
}

/* ========== Braun SSA: Variable Management ========== */

/* Register a variable by its analyzer-assigned symbol_id.
 * Each unique symbol_id gets exactly one var_id slot.  If the same
 * symbol_id is registered again (e.g. redeclaration in the same scope),
 * the existing var_id is reused.  Different symbol_ids with the same
 * name (shadows) naturally get distinct var_ids because the analyzer
 * assigned them different IDs during scope resolution. */
XR_FUNC int xi_lower_var_create(XiLower *l, uint32_t symbol_id, const char *name,
                                struct XrType *type) {
    XR_DCHECK(name != NULL, "var_create: name is NULL");

    /* If symbol_id is resolved (non-zero), look up by ID. */
    if (symbol_id != 0) {
        for (int i = 0; i < l->var_count; i++) {
            if (l->vars[i].symbol_id == symbol_id)
                return i;
        }
    } else {
        /* Fallback for synthetic variables (no analyzer symbol): match by name. */
        for (int i = l->var_count - 1; i >= 0; i--) {
            if (l->vars[i].symbol_id == 0 && l->vars[i].name && strcmp(l->vars[i].name, name) == 0)
                return i;
        }
    }

    XR_CHECK(l->var_count < (int) XI_NO_VAR_ID, "xi_lower: too many source variables (>65534)");
    xi_lower_grow_vars(l, l->var_count + 1);
    int id = l->var_count++;
    l->vars[id].symbol_id = symbol_id;
    l->vars[id].name = name;
    l->vars[id].type = type;
    return id;
}

/* Find variable by symbol_id (primary) or name (fallback).
 * Returns the var_id, or -1 if not found.
 *
 * Fallback to name match handles two cases:
 *  - Caller has sid=0 (new-expressions, enum-access): searches by name.
 *  - Caller has non-zero sid but no match: variable was created without
 *    an analyzer sid (method params via MethodDeclNode.parameters[]).  */
XR_FUNC int xi_lower_var_find(XiLower *l, uint32_t symbol_id, const char *name) {
    if (symbol_id != 0) {
        for (int i = 0; i < l->var_count; i++) {
            if (l->vars[i].symbol_id == symbol_id)
                return i;
        }
    }
    /* Name-based fallback (needed when sid doesn't match or is 0) */
    if (name) {
        for (int i = l->var_count - 1; i >= 0; i--) {
            if (l->vars[i].name && strcmp(l->vars[i].name, name) == 0)
                return i;
        }
    }
    return -1;
}

static inline XiVarId xi_lower_var_id_or_none(int var_id) {
    return (var_id >= 0 && (uint32_t) var_id <= XI_MAX_VAR_ID) ? (XiVarId) var_id : XI_NO_VAR_ID;
}

/* ========== Top-Level Binding Helpers ========== */

XR_FUNC XiTopBinding xi_lower_find_top_binding(XiLower *l, uint32_t symbol_id, const char *name) {
    XiTopBinding b;
    b.slot = -1;
    b.name = NULL;
    b.type = NULL;
    for (XiLower *p = l->parent; p; p = p->parent) {
        if (!p->is_program)
            continue;
        int var_id = xi_lower_var_find(p, symbol_id, name);
        if (var_id >= 0 && p->shared_map[var_id] >= 0) {
            b.slot = p->shared_map[var_id];
            b.name = p->vars[var_id].name;
            b.type = p->vars[var_id].type;
            return b;
        }
    }
    return b;
}

XR_FUNC XiValue *xi_lower_emit_top_load(XiLower *l, XiTopBinding binding, struct XrType *type) {
    XR_DCHECK(binding.slot >= 0, "xi_lower_emit_top_load: binding has no slot");
    XR_DCHECK(binding.name != NULL, "xi_lower_emit_top_load: binding has no name");
    if (!type)
        type = binding.type;
    if (!type)
        type = l->type_any;
    if (l->repl_mode) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_GLOBAL, type, 0);
        if (v)
            v->aux = (void *) binding.name;
        return v;
    }
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_SHARED, type, 0);
    if (v)
        v->aux_int = binding.slot;
    return v;
}

XR_FUNC XiValue *xi_lower_emit_top_store(XiLower *l, XiTopBinding binding, XiValue *val) {
    XR_DCHECK(binding.slot >= 0, "xi_lower_emit_top_store: binding has no slot");
    XR_DCHECK(binding.name != NULL, "xi_lower_emit_top_store: binding has no name");
    XR_DCHECK(val != NULL, "xi_lower_emit_top_store: NULL val");
    XiValue *store;
    if (l->repl_mode) {
        store = xi_value_new(l->func, l->cur_block, XI_SET_GLOBAL, l->type_unit, 1);
        if (store) {
            store->args[0] = val;
            store->aux = (void *) binding.name;
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
    } else {
        val = xi_lower_wrap_shared_store_copy(l, val);
        store = xi_value_new(l->func, l->cur_block, XI_SET_SHARED, l->type_unit, 1);
        if (store) {
            store->args[0] = val;
            store->aux_int = binding.slot;
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
    }
    return store;
}

/* ========== Upvalue Resolution ========== */

/*
 * Resolve a variable from an enclosing scope, recording captures at each
 * level.  Returns the local upvalue index in the immediate child, or -1
 * if the variable is not found in any ancestor.
 *
 * Algorithm (same as Lua/xray flat-upvalue scheme):
 *   1. Check parent's local variables → capture as SRC_REG.
 *   2. Recursively resolve in grandparent → capture as SRC_UPVAL.
 *   3. Each intermediate level records its own capture entry.
 *
 * For program-level shared variables, the caller uses find_shared_var()
 * to emit XI_GET_SHARED directly (no upvalue capture needed).
 */
XR_FUNC int xi_lower_resolve_upvalue(XiLower *l, uint32_t symbol_id, const char *name,
                                     struct XrType **out_type) {
    XiLower *parent = l->parent;
    if (!parent)
        return -1;

    /* Dedup: if this variable is already captured, return existing index */
    for (uint16_t ci = 0; ci < l->func->ncaptures; ci++) {
        if (l->func->captures[ci].name && strcmp(l->func->captures[ci].name, name) == 0) {
            if (xi_lower_reject_error_type(l, l->func->captures[ci].type, "capture metadata", 0))
                return -1;
            if (out_type)
                *out_type = l->func->captures[ci].type;
            return (int) ci;
        }
    }

    /* Check if the variable exists as a local in the immediate parent */
    int var_id = xi_lower_var_find(parent, symbol_id, name);
    if (var_id >= 0) {
        /* Program-level shared variables are handled via XI_GET_SHARED in
         * lower_variable/lower_assignment. Program-local synthetic temporaries
         * still need ordinary closure capture. */
        if (parent->is_program && parent->shared_map && parent->shared_map[var_id] >= 0)
            return -1;

        struct XrType *capture_type = parent->vars[var_id].type;
        if (xi_lower_reject_error_type(l, capture_type, "capture metadata", 0))
            return -1;

        /* Read the current SSA value from the parent's scope.  The value's
         * register will be resolved at emit time via reg_of(). */
        XiValue *parent_val = xi_lower_braun_read(parent, var_id, parent->cur_block);
        if (l->func->ncaptures >= XI_MAX_CAPTURES)
            return -1;
        int idx = l->func->ncaptures;
        l->func->captures[idx].source = XI_CAPTURE_SRC_REG;
        l->func->captures[idx].index = 0;
        l->func->captures[idx].name = name;
        l->func->captures[idx].type = capture_type;
        l->func->captures[idx].value = parent_val;
        l->func->captures[idx].cell_index = -1;
        l->func->captures[idx].env_offset = -1;
        l->func->captures[idx].is_reassigned = false;
        l->func->captures[idx].is_shared = false;
        /* Cell indirection is needed when the capture cannot see the final
         * value at closure creation time:
         *  - Hoisted function variables: initially null, replaced by the
         *    actual closure later.
         *  - Variables captured during function hoisting that have no real
         *    definition yet (braun_read returned a null placeholder): the
         *    actual initializer runs after the closure is created. */
        bool forward_ref =
            (parent_val && parent_val->op == XI_CONST && parent_val->type == parent->type_null);
        l->func->captures[idx].needs_cell = parent->vars[var_id].hoisted || forward_ref;
        l->func->ncaptures++;
        if (out_type)
            *out_type = capture_type;
        return idx;
    }

    /* Not a local in parent — try grandparent (transitive capture) */
    bool parent_had_error_before = parent->had_error;
    int parent_upval = xi_lower_resolve_upvalue(parent, symbol_id, name, out_type);
    if (parent->had_error && !parent_had_error_before) {
        l->had_error = true;
        return -1;
    }
    if (parent_upval >= 0) {
        struct XrType *capture_type = out_type ? *out_type : l->type_any;
        if (xi_lower_reject_error_type(l, capture_type, "capture metadata", 0))
            return -1;
        if (l->func->ncaptures >= XI_MAX_CAPTURES)
            return -1;
        int idx = l->func->ncaptures;
        l->func->captures[idx].source = XI_CAPTURE_SRC_UPVAL;
        l->func->captures[idx].index = (uint16_t) parent_upval;
        l->func->captures[idx].name = name;
        l->func->captures[idx].type = capture_type;
        l->func->captures[idx].cell_index = -1;
        l->func->captures[idx].env_offset = -1;
        l->func->captures[idx].is_reassigned = false;
        l->func->captures[idx].is_shared = false;
        /* Inherit needs_cell from the parent capture so CELL_GET is emitted
         * at every level in the transitive capture chain. */
        if (parent_upval < (int) parent->func->ncaptures)
            l->func->captures[idx].needs_cell = parent->func->captures[parent_upval].needs_cell;
        l->func->ncaptures++;
        return idx;
    }

    return -1;
}

/* Write: currentDef[var][block] = value */
XR_FUNC void xi_lower_braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val) {
    XR_DCHECK(var_id >= 0 && var_id < l->var_cap, "braun_write: var_id out of range");
    if (blk->id >= (uint32_t) l->block_cap)
        xi_lower_grow_blocks(l, (int) blk->id + 1);
    l->var_defs[(size_t) var_id * (size_t) l->block_cap + blk->id] = val;
    /* Tag value with source variable for register coalescing.
     * Skip if the value already belongs to a different variable:
     * overwriting would merge two unrelated variables onto one
     * physical register, corrupting phi operands at loop edges. */
    XiVarId xid = xi_lower_var_id_or_none(var_id);
    if (val && xi_var_id_is_valid(xid)) {
        if (!xi_var_id_is_valid(val->var_id) || val->var_id == xid)
            val->var_id = xid;
        /* Definitions of variables captured by hoisted children must survive
         * DCE: the emitter redirects them through CELL_SET at emit time. */
        if (var_id < l->var_count && l->vars[var_id].captured_by_child)
            val->flags |= XI_FLAG_SIDE_EFFECT;
    }
}

/* Read: get currentDef[var][block], may be NULL. */
static XiValue *braun_read_local(XiLower *l, int var_id, XiBlock *blk) {
    XR_DCHECK(var_id >= 0 && var_id < l->var_cap, "braun_read_local: var_id out of range");
    /* A block beyond the current map has had no def written (writes grow the
     * block dimension), so there is no local def there. */
    if (blk->id >= (uint32_t) l->block_cap)
        return NULL;
    return l->var_defs[(size_t) var_id * (size_t) l->block_cap + blk->id];
}

/* Forward declarations */
static XiValue *braun_read_recursive(XiLower *l, int var_id, XiBlock *blk);
static XiValue *add_phi_operands(XiLower *l, int var_id, XiPhi *phi);

XR_FUNC XiValue *xi_lower_braun_read(XiLower *l, int var_id, XiBlock *blk) {
    XiValue *val = braun_read_local(l, var_id, blk);
    if (val)
        return val;
    return braun_read_recursive(l, var_id, blk);
}

XR_FUNC bool xi_lower_capture_source_vars(XiLower *l) {
    if (!l || !l->func)
        return false;
    if (l->var_count <= 0) {
        l->func->source_var_count = 0;
        return true;
    }

    for (int i = 0; i < l->var_count; i++) {
        if (xi_lower_reject_error_type(l, l->vars[i].type, "source variable metadata", 0)) {
            l->func->source_var_count = 0;
            l->func->source_var_names = NULL;
            l->func->source_var_types = NULL;
            return false;
        }
    }

    uint32_t count = (uint32_t) l->var_count;
    const char **names =
        (const char **) xi_func_arena_alloc(l->func, count * (uint32_t) sizeof(const char *));
    struct XrType **types =
        (struct XrType **) xi_func_arena_alloc(l->func, count * (uint32_t) sizeof(struct XrType *));
    if (!names || !types) {
        l->func->source_var_count = 0;
        l->func->source_var_names = NULL;
        l->func->source_var_types = NULL;
        return true;
    }

    for (uint32_t i = 0; i < count; i++) {
        names[i] = arena_strdup(l->func, l->vars[i].name);
        types[i] = l->vars[i].type;
    }

    l->func->source_var_count = count;
    l->func->source_var_names = names;
    l->func->source_var_types = types;
    return true;
}

/* Try to remove trivial phi: if all operands are the same (or self),
 * replace with that single value. */
static XiValue *try_remove_trivial_phi(XiLower *l, int var_id, XiPhi *phi) {
    XiValue *same = NULL;
    XiValue *pv = &phi->value;

    for (uint16_t i = 0; i < pv->nargs; i++) {
        XiValue *op = pv->args[i];
        if (op == same || op == pv)
            continue; /* self-reference or same as current candidate */
        if (same != NULL)
            return pv; /* non-trivial: two distinct operands */
        same = op;
    }

    if (same == NULL)
        return pv; /* undefined — keep the phi */

    /* Trivial: update the def map so future reads see the simplified value */
    xi_lower_braun_write(l, var_id, phi->value.block, same);
    return same;
}

/*
 * Braun read recursive — the core SSA construction algorithm.
 *
 * Three cases:
 *   1. Block not sealed (loop header): create an incomplete phi, record it,
 *      and fill operands later when the block is sealed.
 *   2. Single predecessor: just recurse into that predecessor.
 *   3. Multiple predecessors (sealed): create phi, fill operands, simplify.
 */
static XiValue *braun_read_recursive(XiLower *l, int var_id, XiBlock *blk) {
    XiValue *val;
    struct XrType *type = l->vars[var_id].type;
    if (!type)
        type = l->type_any;

    if (!blk->sealed) {
        /* Block not sealed: create an incomplete phi placeholder.
         * Operands will be filled in braun_seal_block(). */
        XiPhi *phi = xi_phi_new(l->func, blk, type, 0);
        phi->value.var_id = xi_lower_var_id_or_none(var_id);
        val = &phi->value;

        /* Record for later completion */
        XR_CHECK(l->incomplete_count < XI_LOWER_MAX_INCOMPLETE,
                 "xi_lower: too many incomplete phis");
        XiIncompletePhi *ip = &l->incomplete[l->incomplete_count++];
        ip->var_id = var_id;
        ip->block = blk;
        ip->phi = phi;
    } else if (blk->npreds == 0) {
        /* Entry block or unreachable — variable used before definition. */
        val = xi_const_null(l->func, blk, l->type_null);
        if (val)
            val->var_id = xi_lower_var_id_or_none(var_id);
    } else if (blk->npreds == 1) {
        /* Single predecessor: no phi needed, recurse. */
        val = xi_lower_braun_read(l, var_id, blk->preds[0]);
    } else {
        /* Multiple predecessors: insert phi, then fill operands. */
        XiPhi *phi = xi_phi_new(l->func, blk, type, blk->npreds);
        phi->value.var_id = xi_lower_var_id_or_none(var_id);
        /* Write before filling to break recursive cycles */
        xi_lower_braun_write(l, var_id, blk, &phi->value);
        val = add_phi_operands(l, var_id, phi);
    }

    xi_lower_braun_write(l, var_id, blk, val);
    return val;
}

/* Fill phi operands by reading from each predecessor. */
static XiValue *add_phi_operands(XiLower *l, int var_id, XiPhi *phi) {
    XiBlock *blk = phi->value.block;
    /* Reallocate args to match current pred count */
    phi->value.nargs = blk->npreds;
    if (blk->npreds > 0) {
        phi->value.args =
            (XiValue **) xi_func_arena_alloc(l->func, blk->npreds * sizeof(XiValue *));
    }
    for (uint16_t i = 0; i < blk->npreds; i++) {
        phi->value.args[i] = xi_lower_braun_read(l, var_id, blk->preds[i]);
    }
    return try_remove_trivial_phi(l, var_id, phi);
}

/*
 * Seal a block: all predecessors are now known.
 * Complete any incomplete phis that were deferred.
 */
XR_FUNC void xi_lower_braun_seal(XiLower *l, XiBlock *blk) {
    blk->sealed = true;

    /* Complete all incomplete phis for this block */
    int kept = 0;
    for (int i = 0; i < l->incomplete_count; i++) {
        XiIncompletePhi *ip = &l->incomplete[i];
        if (ip->block == blk) {
            add_phi_operands(l, ip->var_id, ip->phi);
            /* consumed — don't keep */
        } else {
            l->incomplete[kept++] = *ip;
        }
    }
    l->incomplete_count = kept;
}

/* ========== Type Helpers ========== */

/* Get the XrType* for an AST node from the analyzer's side table.
 * Falls back to XR_KIND_UNKNOWN only as last resort. */
XR_FUNC struct XrType *xi_lower_node_type(XiLower *l, AstNode *node) {
    struct XrType *t = xa_analyzer_get_node_type(l->analyzer, node);
    return xi_lower_type_or_any(l, t, "AST node type", node ? node->line : 0);
}

/* ========== Context Initialization ========== */

XR_FUNC void xi_lower_init(XiLower *l, struct XaAnalyzer *analyzer, struct XrVMRuntime *isolate) {
    memset(l, 0, sizeof(XiLower));
    l->analyzer = analyzer;
    l->isolate = isolate;
    l->self_var_id = -1;

    /* Allocate the variable/block maps at their initial capacity; they grow
     * on demand (xi_lower_grow_vars / _blocks) for large functions/modules. */
    l->var_cap = XI_LOWER_INIT_VARS;
    l->block_cap = XI_LOWER_INIT_BLOCKS;
    l->vars = (XiVarEntry *) xr_calloc((size_t) l->var_cap, sizeof(XiVarEntry));
    l->shared_map = (int16_t *) xr_malloc((size_t) l->var_cap * sizeof(int16_t));
    l->shared_slot_funcs =
        (struct XiFunc **) xr_calloc((size_t) l->var_cap, sizeof(struct XiFunc *));
    l->shared_slot_classes =
        (struct XiClassData **) xr_calloc((size_t) l->var_cap, sizeof(struct XiClassData *));
    l->shared_slot_enums = (XiEnumData **) xr_calloc((size_t) l->var_cap, sizeof(XiEnumData *));
    l->shared_slot_imports = (XiImportRef **) xr_calloc((size_t) l->var_cap, sizeof(XiImportRef *));
    l->var_defs =
        (XiValue **) xr_calloc((size_t) l->var_cap * (size_t) l->block_cap, sizeof(XiValue *));
    XR_CHECK(l->vars && l->shared_map && l->shared_slot_funcs && l->shared_slot_classes &&
                 l->shared_slot_enums && l->shared_slot_imports && l->var_defs,
             "xi_lower: failed to allocate variable maps");

    /* Initialize shared_map to -1 (no shared index) */
    for (int i = 0; i < l->var_cap; i++)
        l->shared_map[i] = -1;

    /* Cache singleton types */
    l->type_int = xr_type_new_int(isolate);
    l->type_float = xr_type_new_float(isolate);
    l->type_bool = xr_type_new_bool(isolate);
    l->type_string = xr_type_new_string(isolate);
    l->type_rune = xr_type_new_rune(isolate);
    l->type_null = xr_type_new_null(isolate);
    l->type_unit = xr_type_new_unit(isolate);
    l->type_any = xr_type_new_unknown(isolate);
    l->type_bigint = xr_type_new_bigint(isolate);
    l->type_regex = xr_type_new_regex(isolate);
}

XR_FUNC void xi_lower_cleanup(XiLower *l) {
    xr_free(l->var_defs);
    l->var_defs = NULL;
    xr_free(l->vars);
    l->vars = NULL;
    xr_free(l->shared_map);
    l->shared_map = NULL;
    xr_free(l->shared_slot_funcs);
    l->shared_slot_funcs = NULL;
    xr_free(l->shared_slot_classes);
    l->shared_slot_classes = NULL;
    xr_free(l->shared_slot_enums);
    l->shared_slot_enums = NULL;
    xr_free(l->shared_slot_imports);
    l->shared_slot_imports = NULL;
    xr_free(l->global_asm_templates);
    l->global_asm_templates = NULL;
    l->global_asm_count = 0;
    l->global_asm_cap = 0;
}

XR_FUNC void xi_lower_inherit_evidence(XiLower *child, const XiLower *parent) {
    if (!child || !parent)
        return;
    child->global_evidence = parent->global_evidence;
    child->xg_module_id = parent->xg_module_id;
}

static bool xi_lower_evidence_module_matches(const XiLower *l, XgModuleId module_id) {
    return l && l->xg_module_id != 0 && module_id == l->xg_module_id;
}

XR_FUNC uint32_t xi_lower_source_node_id(const XiLower *l, const AstNode *node) {
    const AstNode *loc;
    uint32_t line;
    uint32_t column;
    if (!l || !node)
        return 0;
    loc = node;
    if (node->type == AST_CALL_EXPR && node->as.call_expr.callee)
        loc = node->as.call_expr.callee;
    line = loc && loc->line > 0 ? (uint32_t) loc->line : (uint32_t) node->line;
    if (line == 0 && loc && loc->end_line > 0)
        line = (uint32_t) loc->end_line;
    column = loc && loc->column > 0 ? (uint32_t) loc->column : (uint32_t) node->column;
    if (column == 0 && loc && loc->end_column > 0)
        column = (uint32_t) loc->end_column;
    if (column == 0)
        column = 1;
    return xg_stable_source_node_id(l->xg_module_id, (uint32_t) node->type, line, column);
}

static XgFuncId xi_lower_find_unique_body_id(XiLower *l, uint8_t kind, uint32_t source_node_id) {
    const XgGlobalEvidence *ev;
    XgFuncId match = XG_NO_ID;
    if (!l || !l->global_evidence || (kind != XG_BODY_MODULE_INIT && source_node_id == 0))
        return XG_NO_ID;
    ev = l->global_evidence;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (body->func_id == XG_NO_ID || body->kind != kind)
            continue;
        if (!xi_lower_evidence_module_matches(l, body->module_id))
            continue;
        if (kind != XG_BODY_MODULE_INIT && body->source_node_id != source_node_id)
            continue;
        if (match != XG_NO_ID)
            return XG_NO_ID;
        match = body->func_id;
    }
    return match;
}

static uint32_t xi_lower_function_evidence_source_node_id(XiLower *l, const AstNode *func_node,
                                                          const FunctionDeclNode *fdecl) {
    uint32_t fallback = xi_lower_source_node_id(l, func_node);
    const XgGlobalEvidence *ev = l ? l->global_evidence : NULL;
    const char *name;
    uint32_t name_id;
    uint32_t source_span_id;
    const XgDeclSummary *decl_match = NULL;
    const XgBodySummary *body_match = NULL;
    if (!l || !func_node || !fdecl || !ev)
        return fallback;
    name = fdecl->name && fdecl->name[0] ? fdecl->name : "<anonymous>";
    name_id = xg_name_id(name);
    source_span_id = (uint32_t) func_node->line;
    if (name_id == 0 || source_span_id == 0)
        return fallback;

    for (uint32_t i = 0; i < ev->ndecls; i++) {
        const XgDeclSummary *decl = &ev->decls[i];
        if (decl->kind != XG_DECL_FUNC || decl->name_id != name_id ||
            decl->source_span_id != source_span_id ||
            !xi_lower_evidence_module_matches(l, decl->module_id))
            continue;
        if (decl_match)
            return fallback;
        decl_match = decl;
    }
    if (!decl_match)
        return fallback;

    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (body->kind != XG_BODY_FUNCTION || body->owner_decl_id != decl_match->decl_id ||
            body->name_id != name_id || !xi_lower_evidence_module_matches(l, body->module_id))
            continue;
        if (body_match)
            return fallback;
        body_match = body;
    }
    if (body_match && body_match->source_node_id != 0)
        return body_match->source_node_id;
    return decl_match->source_node_id != 0 ? decl_match->source_node_id : fallback;
}

static void xi_lower_bind_current_func_body_id(XiLower *l, XgFuncId body_func_id) {
    if (!l || !l->func || body_func_id == XG_NO_ID)
        return;
    if (l->func->xg_body_func_id == XG_NO_ID)
        l->func->xg_body_func_id = body_func_id;
}

XR_FUNC void xi_lower_bind_module_body_id(XiLower *l) {
    xi_lower_bind_current_func_body_id(l, xi_lower_find_unique_body_id(l, XG_BODY_MODULE_INIT, 0));
}

XR_FUNC void xi_lower_bind_function_body_id(XiLower *l, uint32_t source_node_id) {
    xi_lower_bind_current_func_body_id(
        l, xi_lower_find_unique_body_id(l, XG_BODY_FUNCTION, source_node_id));
}

XR_FUNC void xi_lower_bind_method_body_id(XiLower *l, uint32_t source_node_id) {
    xi_lower_bind_current_func_body_id(
        l, xi_lower_find_unique_body_id(l, XG_BODY_METHOD, source_node_id));
}

XR_FUNC uint32_t xi_lower_next_key_access_ordinal(XiLower *l, uint32_t source_span_id,
                                                  uint8_t access_op) {
    const XgGlobalEvidence *ev;
    uint32_t ordinal;
    if (!l || !l->global_evidence || !l->func || l->func->xg_body_func_id == XG_NO_ID)
        return UINT32_MAX;
    ev = l->global_evidence;
    ordinal = l->xg_next_key_access_ordinal;
    for (uint32_t i = 0; i < ev->nkey_accesses; i++) {
        const XgKeyAccessSummary *row = &ev->key_accesses[i];
        if (row->owner_func_id != (XgFuncId) l->func->xg_body_func_id)
            continue;
        if (row->body_ordinal != ordinal)
            continue;
        if (row->source_span_id == source_span_id && row->op == access_op) {
            l->xg_next_key_access_ordinal++;
            return row->body_ordinal;
        }
        return UINT32_MAX;
    }
    return UINT32_MAX;
}

XR_FUNC void xi_lower_bind_callsite_id(XiLower *l, XiValue *call, uint32_t source_node_id) {
    const XgGlobalEvidence *ev;
    const XgCallsiteSummary *match = NULL;
    if (!l || !call || !l->global_evidence || !l->func || l->func->xg_body_func_id == XG_NO_ID ||
        source_node_id == 0 ||
        (call->op != XI_CALL && call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs == 0)
        return;
    ev = l->global_evidence;
    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *row = &ev->callsites[i];
        if (row->owner_func_id != (XgFuncId) l->func->xg_body_func_id ||
            row->source_node_id != source_node_id)
            continue;
        if (call->op == XI_CALL) {
            if (row->kind != XG_CALL_DIRECT_FUNC && row->kind != XG_CALL_NATIVE &&
                row->kind != XG_CALL_EXTERN && row->kind != XG_CALL_CLOSURE)
                continue;
        } else {
            if (row->kind != XG_CALL_METHOD && row->kind != XG_CALL_INTERFACE)
                continue;
        }
        if (match)
            return;
        match = row;
    }
    if (match) {
        call->xg_callsite_id = match->callsite_id;
        if (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT ||
            match->kind == XG_CALL_NATIVE || match->kind == XG_CALL_EXTERN)
            call->xg_method_id = match->method_id;
    }
}

static const XgClassSummary *xi_lower_find_class_by_id(const XgGlobalEvidence *ev,
                                                       XgClassId class_id) {
    if (!ev || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *cls = &ev->classes[i];
        if (cls->class_id == class_id)
            return cls;
    }
    return NULL;
}

static const XgClassSummary *xi_lower_find_unique_class_by_name_id(const XgGlobalEvidence *ev,
                                                                   uint32_t name_id,
                                                                   bool generic_origin) {
    const XgClassSummary *match = NULL;
    if (!ev || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *cls = &ev->classes[i];
        uint32_t candidate = generic_origin ? cls->generic_origin_name_id : cls->name_id;
        if (candidate != name_id)
            continue;
        if (match)
            return NULL;
        match = cls;
    }
    return match;
}

static const XgClassSummary *xi_lower_find_unique_class_by_name(const XgGlobalEvidence *ev,
                                                                const char *class_name) {
    uint32_t name_id = class_name ? xg_name_id(class_name) : 0;
    const XgClassSummary *match;
    if (name_id == 0)
        return NULL;
    match = xi_lower_find_unique_class_by_name_id(ev, name_id, false);
    if (match)
        return match;
    return xi_lower_find_unique_class_by_name_id(ev, name_id, true);
}

static const XgClassSummary *
xi_lower_find_unique_class_by_receiver_type(const XgGlobalEvidence *ev,
                                            const struct XrType *receiver_type) {
    const XgClassSummary *match;
    if (!ev || !receiver_type)
        return NULL;
    match = xi_lower_find_unique_class_by_name(ev, receiver_type->instance.class_name);
    if (match)
        return match;
    if (receiver_type->instance.class_ref && receiver_type->instance.class_ref->name) {
        match = xi_lower_find_unique_class_by_name(ev, receiver_type->instance.class_ref->name);
        if (match)
            return match;
    }
    const char *class_name = receiver_type->instance.class_name;
    const char *tail = class_name ? strrchr(class_name, '.') : NULL;
    if (tail && tail[1] != '\0')
        return xi_lower_find_unique_class_by_name(ev, tail + 1);
    return NULL;
}

static const XgClassFieldSummary *xi_lower_find_unique_own_class_field(const XgGlobalEvidence *ev,
                                                                       const XgClassSummary *cls,
                                                                       uint32_t field_name_id) {
    const XgClassFieldSummary *match = NULL;
    uint32_t start;
    if (!ev || !cls || field_name_id == 0 || cls->field_count == 0)
        return NULL;
    if (cls->field_start == 0)
        return NULL;
    start = cls->field_start - 1;
    if (start >= ev->nclass_fields || cls->field_count > ev->nclass_fields - start)
        return NULL;
    for (uint32_t i = 0; i < cls->field_count; i++) {
        const XgClassFieldSummary *field = &ev->class_fields[start + i];
        if (field->owner_class_id != cls->class_id || field->name_id != field_name_id ||
            (field->flags & XG_CLASS_FIELD_STATIC) != 0)
            continue;
        if (match)
            return NULL;
        match = field;
    }
    return match;
}

XR_FUNC void xi_lower_bind_class_field_id(XiLower *l, XiValue *access,
                                          const struct XrType *receiver_type,
                                          const char *field_name) {
    const XgGlobalEvidence *ev;
    const XgClassSummary *cls;
    uint32_t field_name_id;
    if (!l || !access || !l->global_evidence || !receiver_type || !field_name ||
        (access->op != XI_LOAD_FIELD && access->op != XI_STORE_FIELD))
        return;
    if (receiver_type->kind != XR_KIND_CLASS && receiver_type->kind != XR_KIND_INSTANCE)
        return;
    if (!receiver_type->instance.class_name && !receiver_type->instance.class_ref)
        return;
    field_name_id = xg_name_id(field_name);
    if (field_name_id == 0)
        return;
    ev = l->global_evidence;
    cls = xi_lower_find_unique_class_by_receiver_type(ev, receiver_type);
    for (uint32_t depth = 0; cls && depth < 64; depth++) {
        const XgClassFieldSummary *field =
            xi_lower_find_unique_own_class_field(ev, cls, field_name_id);
        if (field) {
            access->xg_class_field_id = field->field_id;
            return;
        }
        if (cls->parent_class_id == XG_NO_ID)
            return;
        cls = xi_lower_find_class_by_id(ev, cls->parent_class_id);
    }
}

static bool xi_lower_json_access_row_requires_dynamic_lookup(const XgGlobalEvidence *ev,
                                                             const XgJsonAccessSummary *row);

XR_FUNC void xi_lower_bind_json_access_id(XiLower *l, XiValue *access, const char *field_name,
                                          uint32_t source_span_id, uint16_t field_ordinal,
                                          uint8_t access_kind) {
    const XgGlobalEvidence *ev;
    const XgJsonAccessSummary *match = NULL;
    uint32_t key_name_id;
    if (!l || !access || !l->global_evidence || !l->func || l->func->xg_body_func_id == XG_NO_ID ||
        (access->op != XI_JSON_GET_F && access->op != XI_JSON_SET_F && access->op != XI_INDEX_GET &&
         access->op != XI_INDEX_SET && access->op != XI_LOAD_FIELD && access->op != XI_STORE_FIELD))
        return;
    key_name_id = field_name ? xg_name_id(field_name) : 0;
    if (key_name_id == 0 && access_kind != XG_JSON_ACCESS_INDEX_GET &&
        access_kind != XG_JSON_ACCESS_INDEX_SET)
        return;
    ev = l->global_evidence;
    for (uint32_t i = 0; i < ev->njson_accesses; i++) {
        const XgJsonAccessSummary *row = &ev->json_accesses[i];
        if (row->owner_func_id != (XgFuncId) l->func->xg_body_func_id)
            continue;
        if (!xi_lower_evidence_module_matches(l, row->module_id))
            continue;
        bool field_matches = row->field_ordinal == field_ordinal;
        if (!field_matches && field_ordinal == UINT16_MAX &&
            (access->op == XI_INDEX_GET || access->op == XI_INDEX_SET ||
             access->op == XI_LOAD_FIELD || access->op == XI_STORE_FIELD))
            field_matches = xi_lower_json_access_row_requires_dynamic_lookup(ev, row);
        if (row->source_span_id != source_span_id || row->key_name_id != key_name_id ||
            !field_matches || row->access_kind != access_kind)
            continue;
        if (match)
            return;
        match = row;
    }
    if (match)
        access->xg_json_access_id = match->json_access_id;
}

static bool xi_lower_json_access_row_requires_dynamic_lookup(const XgGlobalEvidence *ev,
                                                             const XgJsonAccessSummary *row) {
    const XgJsonShapeSummary *shape;
    if (!ev || !row)
        return false;
    if ((row->flags & XG_JSON_ACCESS_COMPUTED_KEY) != 0 || row->key_name_id == 0 ||
        row->receiver_shape_id == XG_NO_ID)
        return true;
    shape = xg_global_evidence_find_json_shape(ev, row->receiver_shape_id);
    return !shape || shape->shape_kind == XG_JSON_SHAPE_OPEN ||
           row->field_ordinal >= shape->field_count;
}

static bool xi_lower_json_access_row_is_direct_index(const XgGlobalEvidence *ev,
                                                     const XgJsonAccessSummary *row) {
    if (!row || (row->flags & XG_JSON_ACCESS_RECEIVER_SHAPE_PROVEN) == 0)
        return false;
    return !xi_lower_json_access_row_requires_dynamic_lookup(ev, row);
}

XR_FUNC bool xi_lower_find_json_direct_field_ordinal(XiLower *l, const char *field_name,
                                                     uint32_t source_span_id, uint8_t access_kind,
                                                     uint16_t *out_ordinal) {
    const XgGlobalEvidence *ev;
    uint32_t key_name_id;
    XgFuncId owner_func_id = XG_NO_ID;
    bool owner_seen = false;
    bool owner_direct_seen = false;
    bool owner_ambiguous = false;
    uint16_t owner_ordinal = UINT16_MAX;
    bool fallback_seen = false;
    bool fallback_direct_seen = false;
    bool fallback_ambiguous = false;
    uint16_t fallback_ordinal = UINT16_MAX;
    if (!out_ordinal)
        return false;
    *out_ordinal = UINT16_MAX;
    if (!l || !l->global_evidence || !field_name)
        return false;
    key_name_id = xg_name_id(field_name);
    if (key_name_id == 0)
        return false;
    ev = l->global_evidence;
    if (l->func)
        owner_func_id = (XgFuncId) l->func->xg_body_func_id;
    for (uint32_t i = 0; i < ev->njson_accesses; i++) {
        const XgJsonAccessSummary *row = &ev->json_accesses[i];
        bool direct;
        if (!xi_lower_evidence_module_matches(l, row->module_id))
            continue;
        if (row->source_span_id != source_span_id || row->key_name_id != key_name_id ||
            row->access_kind != access_kind)
            continue;
        direct = xi_lower_json_access_row_is_direct_index(ev, row);
        if (owner_func_id != XG_NO_ID && row->owner_func_id == owner_func_id) {
            owner_seen = true;
            if (!direct) {
                owner_ambiguous = true;
                continue;
            }
            if (owner_direct_seen)
                owner_ambiguous = true;
            owner_direct_seen = true;
            owner_ordinal = row->field_ordinal;
            continue;
        }
        fallback_seen = true;
        if (!direct) {
            fallback_ambiguous = true;
            continue;
        }
        if (fallback_direct_seen)
            fallback_ambiguous = true;
        fallback_direct_seen = true;
        fallback_ordinal = row->field_ordinal;
    }
    if (owner_seen) {
        if (owner_direct_seen && !owner_ambiguous) {
            *out_ordinal = owner_ordinal;
            return true;
        }
        return false;
    }
    if (fallback_seen && fallback_direct_seen && !fallback_ambiguous) {
        *out_ordinal = fallback_ordinal;
        return true;
    }
    return false;
}

XR_FUNC bool xi_lower_json_access_requires_dynamic_lookup(XiLower *l, const char *field_name,
                                                          uint32_t source_span_id,
                                                          uint16_t field_ordinal,
                                                          uint8_t access_kind) {
    const XgGlobalEvidence *ev;
    bool has_owner_match = false;
    bool has_fallback_match = false;
    bool owner_requires_dynamic = false;
    bool fallback_requires_dynamic = false;
    uint32_t key_name_id;
    XgFuncId owner_func_id = XG_NO_ID;
    if (!l || !l->global_evidence)
        return false;
    key_name_id = field_name ? xg_name_id(field_name) : 0;
    if (key_name_id == 0 && access_kind != XG_JSON_ACCESS_INDEX_GET &&
        access_kind != XG_JSON_ACCESS_INDEX_SET)
        return false;
    ev = l->global_evidence;
    if (l->func)
        owner_func_id = (XgFuncId) l->func->xg_body_func_id;
    for (uint32_t i = 0; i < ev->njson_accesses; i++) {
        const XgJsonAccessSummary *row = &ev->json_accesses[i];
        if (!xi_lower_evidence_module_matches(l, row->module_id))
            continue;
        if (row->source_span_id != source_span_id || row->key_name_id != key_name_id ||
            row->field_ordinal != field_ordinal || row->access_kind != access_kind)
            continue;
        if (owner_func_id != XG_NO_ID && row->owner_func_id == owner_func_id) {
            has_owner_match = true;
            if (xi_lower_json_access_row_requires_dynamic_lookup(ev, row))
                owner_requires_dynamic = true;
        } else {
            has_fallback_match = true;
            if (xi_lower_json_access_row_requires_dynamic_lookup(ev, row))
                fallback_requires_dynamic = true;
        }
    }
    if (has_owner_match)
        return owner_requires_dynamic;
    return has_fallback_match && fallback_requires_dynamic;
}

XR_FUNC void xi_lower_bind_record_access_id(XiLower *l, XiValue *access, const char *field_name,
                                            uint32_t source_span_id, uint16_t field_ordinal,
                                            uint8_t access_kind) {
    const XgGlobalEvidence *ev;
    const XgRecordAccessSummary *match = NULL;
    uint32_t field_name_id;
    if (!l || !access || !l->global_evidence || !l->func || l->func->xg_body_func_id == XG_NO_ID ||
        (access->op != XI_JSON_GET_F && access->op != XI_JSON_SET_F))
        return;
    field_name_id = field_name ? xg_name_id(field_name) : 0;
    if (field_name_id == 0)
        return;
    ev = l->global_evidence;
    for (uint32_t i = 0; i < ev->nrecord_accesses; i++) {
        const XgRecordAccessSummary *row = &ev->record_accesses[i];
        if (row->owner_func_id != (XgFuncId) l->func->xg_body_func_id)
            continue;
        if (!xi_lower_evidence_module_matches(l, row->module_id))
            continue;
        if (row->source_span_id != source_span_id || row->field_name_id != field_name_id ||
            row->field_ordinal != field_ordinal || row->access_kind != access_kind)
            continue;
        if (match)
            return;
        match = row;
    }
    if (match)
        access->xg_record_access_id = match->record_access_id;
}

XR_FUNC void xi_lower_bind_key_access_id(XiLower *l, XiValue *access, uint32_t source_span_id,
                                         uint32_t body_ordinal, uint8_t access_op) {
    const XgGlobalEvidence *ev;
    const XgKeyAccessSummary *match = NULL;
    if (!l || !access || body_ordinal == UINT32_MAX || !l->global_evidence || !l->func ||
        l->func->xg_body_func_id == XG_NO_ID ||
        (access->op != XI_INDEX_GET && access->op != XI_INDEX_SET && access->op != XI_CALL_METHOD &&
         access->op != XI_CALL_METHOD_DIRECT))
        return;
    ev = l->global_evidence;
    for (uint32_t i = 0; i < ev->nkey_accesses; i++) {
        const XgKeyAccessSummary *row = &ev->key_accesses[i];
        if (row->owner_func_id != (XgFuncId) l->func->xg_body_func_id)
            continue;
        if (row->source_span_id != source_span_id || row->body_ordinal != body_ordinal ||
            row->op != access_op)
            continue;
        if (match)
            return;
        match = row;
    }
    if (match)
        access->xg_key_access_id = match->access_id;
}

XR_FUNC void xi_lower_take_sequence_evidence_ids(XiLower *l, uint32_t source_span_id,
                                                 XiSequenceEvidenceKinds kinds,
                                                 XiSequenceEvidenceIds *out_ids) {
    const XgGlobalEvidence *ev;
    XgFuncId owner_func_id;
    if (!out_ids)
        return;
    memset(out_ids, 0, sizeof(*out_ids));
    if (!l || !l->global_evidence || !l->func || l->func->xg_body_func_id == XG_NO_ID)
        return;
    ev = l->global_evidence;
    owner_func_id = (XgFuncId) l->func->xg_body_func_id;

    if (kinds.sequence_access_kind != 0) {
        for (uint32_t i = 0; i < ev->nsequence_accesses; i++) {
            const XgSequenceAccessSummary *row = &ev->sequence_accesses[i];
            if (row->owner_func_id != owner_func_id ||
                row->body_ordinal != l->xg_next_sequence_access_ordinal)
                continue;
            if (row->source_span_id == source_span_id &&
                row->access_kind == kinds.sequence_access_kind) {
                out_ids->sequence_access_id = row->access_id;
                l->xg_next_sequence_access_ordinal++;
            }
            break;
        }
    }
    if (kinds.capacity_op_kind != 0) {
        for (uint32_t i = 0; i < ev->ncapacity_ops; i++) {
            const XgCapacityOpSummary *row = &ev->capacity_ops[i];
            if (row->owner_func_id != owner_func_id ||
                row->body_ordinal != l->xg_next_capacity_op_ordinal)
                continue;
            if (row->source_span_id == source_span_id && row->op_kind == kinds.capacity_op_kind) {
                out_ids->capacity_op_id = row->op_id;
                l->xg_next_capacity_op_ordinal++;
            }
            break;
        }
    }
    if (kinds.bulk_op_kind != 0) {
        for (uint32_t i = 0; i < ev->nbulk_ops; i++) {
            const XgBulkOpSummary *row = &ev->bulk_ops[i];
            if (row->owner_func_id != owner_func_id ||
                row->body_ordinal != l->xg_next_bulk_op_ordinal)
                continue;
            if (row->source_span_id == source_span_id && row->op_kind == kinds.bulk_op_kind) {
                out_ids->bulk_op_id = row->op_id;
                l->xg_next_bulk_op_ordinal++;
            }
            break;
        }
    }
    if (kinds.encoding_op_kind != 0) {
        for (uint32_t i = 0; i < ev->nencoding_ops; i++) {
            const XgEncodingOpSummary *row = &ev->encoding_ops[i];
            if (row->owner_func_id != owner_func_id ||
                row->body_ordinal != l->xg_next_encoding_op_ordinal)
                continue;
            if (row->source_span_id == source_span_id && row->op_kind == kinds.encoding_op_kind) {
                out_ids->encoding_op_id = row->op_id;
                l->xg_next_encoding_op_ordinal++;
            }
            break;
        }
    }
}

XR_FUNC void xi_lower_apply_sequence_evidence_ids(XiValue *value,
                                                  const XiSequenceEvidenceIds *ids) {
    if (!value || !ids)
        return;
    value->xg_sequence_access_id = ids->sequence_access_id;
    value->xg_capacity_op_id = ids->capacity_op_id;
    value->xg_bulk_op_id = ids->bulk_op_id;
    value->xg_encoding_op_id = ids->encoding_op_id;
}

/* ========== Method Symbol Resolution ========== */

XR_FUNC int32_t xi_lower_method_symbol(XiLower *l, const char *method_name) {
    if (!l->isolate || !method_name)
        return 0;
    XrSymbolTable *st = (XrSymbolTable *) xr_isolate_get_symbol_table(l->isolate);
    if (!st)
        return 0;
    return (int32_t) xr_symbol_register_in_table(st, method_name);
}

/* ========== Function Lowering Implementation ========== */

#if XR_DEBUG
/* Defense-in-depth assertion (debug/CI only): on the freshly lowered RAW IR,
 * before any optimization pass runs, every value's var_id must be either
 * XI_NO_VAR_ID or a real index < var_count. Register
 * coalescing in xi_emit (reg_of) keys on var_id, so a stray one silently pins
 * an unrelated value onto a live local's register — the exact failure that made
 * a temporary && / || / ternary phi clobber variable #0. This lives here, NOT
 * in xi_verify: inlining clones callee values keeping their own var_ids (which
 * are out of the caller's range), and xi_verify also runs post-inline, so the
 * bound only holds on the pre-inline IR available at lowering time. */
static void xi_lower_assert_var_ids(const XiLower *l, const XiFunc *f) {
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (!v)
                continue;
            XR_DCHECK_FMT(!xi_var_id_is_valid(v->var_id) || v->var_id < l->var_count,
                          "xi_lower: func '%s' v%u (op=%u) has var_id=%u >= var_count=%d",
                          f->name ? f->name : "?", v->id, v->op, (unsigned) v->var_id,
                          l->var_count);
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            XR_DCHECK_FMT(
                !xi_var_id_is_valid(phi->value.var_id) || phi->value.var_id < l->var_count,
                "xi_lower: func '%s' phi v%u has var_id=%u >= var_count=%d",
                f->name ? f->name : "?", phi->value.id, (unsigned) phi->value.var_id, l->var_count);
        }
    }
}
#else
static inline void xi_lower_assert_var_ids(const XiLower *l, const XiFunc *f) {
    (void) l;
    (void) f;
}
#endif

/*
 * Internal function lowering with optional parent context.
 * When parent is non-NULL, the child can resolve variable references
 * from enclosing scopes via the upvalue capture mechanism.
 */
XR_FUNC XiFunc *xi_lower_func_impl(AstNode *func_node, struct XaAnalyzer *analyzer,
                                   struct XrVMRuntime *isolate, XiLower *parent_ctx) {
    XR_DCHECK(func_node != NULL, "lower_func_impl: func_node is NULL");
    FunctionDeclNode *fdecl = &func_node->as.function_decl;

    XiLower l;
    xi_lower_init(&l, analyzer, isolate);
    l.parent = parent_ctx;
    /* Inherit repl_mode from the enclosing program / function so nested
     * closures resolve free top-level names through the globals dict. */
    if (parent_ctx) {
        l.repl_mode = parent_ctx->repl_mode;
        xi_lower_inherit_evidence(&l, parent_ctx);
    }

    /* Determine return type.  fdecl->return_type is an XrTypeRef (AST
     * syntax); resolve it to a runtime XrType* via the analyzer's
     * resolver — assigning the XrTypeRef directly mixes up two unrelated
     * struct layouts and produces garbage values for every downstream
     * type lookup (AOT codegen RET tag, TFA, etc.). */
    struct XrType *ret_type =
        fdecl->return_type ? xr_tref_resolve(isolate, fdecl->return_type) : NULL;
    if (!ret_type) {
        /* Un-annotated closure/lambda: named functions must annotate their
         * return type, but anonymous functions may leave it to inference. The
         * analyzer records the inferred type on the function-literal node; if
         * we ignore it and fall back to `unit`, AOT emits a `void` closure
         * body that drops the returned value (call sites then see null/0).
         * The VM is unaffected because it is dynamically typed, so this only
         * manifests under AOT. Recover the inferred return type here. */
        struct XrType *node_type = xa_analyzer_get_node_type(analyzer, func_node);
        if (node_type && XR_TYPE_IS_FUNCTION(node_type) && node_type->function.return_type)
            ret_type = node_type->function.return_type;
    }
    if (!ret_type)
        ret_type = l.type_unit;
    if (xi_lower_reject_error_type(&l, ret_type, "function return type", func_node->line)) {
        xi_lower_cleanup(&l);
        return NULL;
    }

    l.func = xi_func_new(fdecl->name && fdecl->name[0] ? fdecl->name : "<anonymous>", ret_type);
    if (!l.func) {
        xi_lower_cleanup(&l);
        return NULL;
    }
    l.func->parent_func = parent_ctx ? parent_ctx->func : NULL;
    l.func->analyzer = analyzer;
    xi_lower_bind_function_body_id(&l,
                                   xi_lower_function_evidence_source_node_id(&l, func_node, fdecl));

    /* FFI: @extern("C") functions are bodyless foreign declarations. Record
     * the C symbol + optional dylib; the body below stays empty and a trivial
     * zero return is synthesized so the IR is well-formed (codegen replaces it
     * with `extern Ret sym(...)` and direct calls). */
    if (fdecl->attributes) {
        for (int i = 0; i < fdecl->attr_count; i++) {
            XrAttribute *a = fdecl->attributes[i];
            if (!a)
                continue;
            if (a->kind == ATTR_EXTERN) {
                l.func->is_extern = true;
                l.func->extern_symbol = fdecl->name;
            } else if (a->kind == ATTR_DYLIB) {
                l.func->extern_dylib = a->str_arg;
            } else if (a->kind == ATTR_C_EXPORT) {
                l.func->c_export = true;
                l.func->c_export_symbol = a->str_arg ? a->str_arg : fdecl->name;
            } else if (a->kind == ATTR_SECTION) {
                l.func->aot_section = a->str_arg;
            } else if (a->kind == ATTR_WEAK) {
                l.func->aot_weak = true;
            } else if (a->kind == ATTR_USED) {
                l.func->aot_used = true;
            } else if (a->kind == ATTR_NAKED) {
                l.func->aot_naked = true;
            } else if (a->kind == ATTR_INTERRUPT) {
                l.func->aot_interrupt_abi = a->str_arg;
            } else if (a->kind == ATTR_NO_ALLOC) {
                l.func->no_alloc = true;
            }
        }
    }

    /* Entry block (no predecessors — seal immediately) */
    XiBlock *entry = xi_block_new(l.func);
    entry->sealed = true;
    l.cur_block = entry;

    /* Lower parameters */
    l.func->nparams = (uint16_t) fdecl->param_count;
    if (fdecl->param_count > 0) {
        l.func->params = (XiValue **) xr_calloc(fdecl->param_count, sizeof(XiValue *));
        if (!l.func->params) {
            xi_func_free(l.func);
            xi_lower_cleanup(&l);
            return NULL;
        }
    }

    /* Detect rest parameter and compute VM entry metadata */
    bool has_rest = false;
    for (int i = 0; i < fdecl->param_count; i++) {
        if (fdecl->params[i] && fdecl->params[i]->is_rest) {
            has_rest = true;
            break;
        }
    }
    l.func->is_vararg = has_rest;
    l.func->min_params = (uint16_t) fdecl->required_count;

    if (fdecl->is_generator) {
        l.func->entry_type = 2; /* XR_ENTRY_GENERATOR */
    } else if (fdecl->required_count < (has_rest ? fdecl->param_count - 1 : fdecl->param_count)) {
        l.func->entry_type = 1; /* XR_ENTRY_DEFAULTS */
    } else {
        l.func->entry_type = 0; /* XR_ENTRY_NORMAL */
    }

    /* nparams excludes rest param (VM packs varargs into the rest slot) */
    if (has_rest) {
        l.func->nparams = (uint16_t) (fdecl->param_count - 1);
    }

    for (int i = 0; i < fdecl->param_count; i++) {
        XrParamNode *p = fdecl->params[i];
        struct XrType *ptype = xi_lower_param_type(&l, p);
        if (xi_lower_reject_error_type(&l, ptype, "function parameter type",
                                       p ? p->line : func_node->line)) {
            xi_func_free(l.func);
            xi_lower_cleanup(&l);
            return NULL;
        }

        XiValue *param_val = xi_param(l.func, entry, (uint16_t) i, ptype);
        l.func->params[i] = param_val;
        if (i < l.func->nparams && p && p->passing_mode != XR_PARAM_VALUE &&
            !xi_func_set_param_passing_mode(l.func, (uint16_t) i, p->passing_mode)) {
            xi_func_free(l.func);
            xi_lower_cleanup(&l);
            return NULL;
        }

        /* Register parameter in Braun SSA using analyzer-assigned symbol_id */
        int var_id = xi_lower_var_create(&l, p->symbol_id, p->name, ptype);
        xi_lower_braun_write(&l, var_id, entry, param_val);
    }

    /* Default parameter values are filled at the call site (C1): the analyzer
     * completes omitted trailing arguments with cloned default expressions, so
     * the callee never sees an omitted argument and the old "null sentinel ->
     * default" entry guard is gone. This keeps an explicit `null` argument
     * distinct from omission and removes a per-call branch from the AOT path. */

    /* For named functions, register a self-reference so the body can
     * resolve recursive calls.  lower_call detects l.self_value and
     * emits a self-call (OP_CALLSELF) instead of a regular call. */
    if (fdecl->name) {
        struct XrType *fn_type = ret_type; /* approximate; exact type unused */
        XiValue *self = xi_const_null(l.func, entry, l.type_null);
        l.self_value = self;
        int self_var = xi_lower_var_create(&l, fdecl->symbol_id, fdecl->name, fn_type);
        l.self_var_id = self_var;
        xi_lower_braun_write(&l, self_var, entry, self);
    }

    /* Propagate @test / @before_each / etc. attributes to XiFunc */
    if (fdecl->attr_count > 0 && fdecl->attributes) {
        for (int i = 0; i < fdecl->attr_count; i++) {
            XrAttribute *a = fdecl->attributes[i];
            if (a && a->kind != ATTR_NONE) {
                l.func->test_attr = (uint8_t) a->kind;
                l.func->test_timeout = a->timeout;
                break;
            }
        }
    }

    /* Lower function body (extern functions are bodyless) */
    xi_lower_defer_scope_push(&l);
    if (fdecl->body) {
        xi_lower_stmt(&l, fdecl->body);
    }
    xi_lower_defer_scope_pop_normal(&l, func_node->line);

    /* If last block not terminated, add implicit return. Extern functions
     * return a zero of the declared type so the synthesized stub type-checks;
     * everything else returns void. */
    if (l.cur_block) {
        if (l.func->is_extern) {
            XrRep rep = xr_type_rep(ret_type);
            XiValue *zero = NULL;
            if (rep == XR_REP_F64)
                zero = xi_const_float(l.func, l.cur_block, 0.0, ret_type);
            else if (rep == XR_REP_I64)
                zero = xi_const_int(l.func, l.cur_block, 0, ret_type);
            else if (rep != XR_REP_VOID)
                zero = xi_const_null(l.func, l.cur_block, ret_type);
            xi_block_set_return(l.cur_block, zero);
        } else {
            xi_block_set_return(l.cur_block, NULL);
        }
    }

    XiFunc *result = NULL;
    if (!l.had_error && xi_lower_capture_source_vars(&l)) {
        result = l.func;
        result->stage = XI_STAGE_RAW;
        result->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);
        xi_lower_assert_var_ids(&l, result);
    }
    xi_lower_cleanup(&l);
    return result;
}

/* ========== Public API ========== */

XR_FUNC XiFunc *xi_lower_func(AstNode *func_node, struct XaAnalyzer *analyzer,
                              struct XrVMRuntime *isolate) {
    XR_CHECK(func_node != NULL, "xi_lower_func: func_node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_func: analyzer is NULL");
    XR_CHECK(func_node->type == AST_FUNCTION_DECL || func_node->type == AST_FUNCTION_EXPR,
             "xi_lower_func: not a function node");
    XiFunc *f = xi_lower_func_impl(func_node, analyzer, isolate, NULL);
    if (f) {
        finalize_capture_metadata(f);
        xi_func_compute_effects(f);
    }
    return f;
}

/* Extract the name, symbol_id, type, and const-ness from a top-level
 * statement.  Returns the statement (unwrapped from export if needed)
 * and sets *out_exported.  Returns NULL for non-declaration statements. */
static AstNode *prescan_extract_decl(XiLower *l, AstNode *s, const char **out_name,
                                     uint32_t *out_sid, struct XrType **out_type,
                                     bool *out_is_const, bool *out_is_exported) {
    *out_name = NULL;
    *out_sid = 0;
    *out_type = l->type_any;
    *out_is_const = false;
    *out_is_exported = false;

    if (!s)
        return NULL;

    if (s->type == AST_EXPORT_STMT) {
        if (!s->as.export_stmt.declaration)
            return s;
        s = s->as.export_stmt.declaration;
        *out_is_exported = true;
    }

    switch (s->type) {
        case AST_FUNCTION_DECL:
            *out_name = s->as.function_decl.name;
            *out_sid = s->as.function_decl.symbol_id;
            *out_type = xi_lower_node_type(l, s);
            break;
        case AST_CLASS_DECL:
            *out_name = s->as.class_decl.name;
            *out_sid = s->as.class_decl.symbol_id;
            break;
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            *out_name = s->as.struct_decl.name;
            *out_sid = s->as.struct_decl.symbol_id;
            break;
        case AST_CONST_DECL:
            *out_is_const = true;
            /* fall through */
        case AST_VAR_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            *out_name = s->as.var_decl.name;
            *out_sid = s->as.var_decl.symbol_id;
            *out_type = xi_lower_node_type(l, s);
            break;
        case AST_ENUM_DECL:
            *out_name = s->as.enum_decl.name;
            *out_sid = s->as.enum_decl.symbol_id;
            break;
        case AST_IMPORT_STMT:
            break;
        default:
            return NULL;
    }
    return s;
}

static bool prescan_is_user_owned_decl(AstNode *s) {
    return s && s->line > 0;
}

static XrAttribute *prescan_var_attr(const VarDeclNode *var, AttributeKind kind) {
    if (!var || !var->attributes)
        return NULL;
    for (int i = 0; i < var->attr_count; i++) {
        if (var->attributes[i] && var->attributes[i]->kind == kind)
            return var->attributes[i];
    }
    return NULL;
}

static void prescan_apply_static_data_attrs(XiLower *l, const VarDeclNode *var,
                                            XiConstLiteral *lit) {
    if (!l || !var || !lit)
        return;
    XrAttribute *section = prescan_var_attr(var, ATTR_SECTION);
    XrAttribute *weak = prescan_var_attr(var, ATTR_WEAK);
    XrAttribute *used = prescan_var_attr(var, ATTR_USED);
    if (section && section->str_arg && section->str_arg[0])
        lit->data_section = arena_strdup(l->func, section->str_arg);
    if (weak)
        lit->data_weak = true;
    if (used)
        lit->data_used = true;
}

typedef struct PrescanSlotMeta {
    const char **export_names;
    const char **owned_names;
    uint8_t *owned_consts;
    XiConstLiteral *const_literals;
    XiConstLiteral *shared_initializers;
    uint16_t cap;
} PrescanSlotMeta;

static bool prescan_slot_meta_reserve(PrescanSlotMeta *m, uint16_t need) {
    if (!m || need <= m->cap)
        return true;
    uint16_t nc = m->cap ? m->cap : 16;
    while (nc < need) {
        if (nc > UINT16_MAX / 2u) {
            nc = need;
            break;
        }
        nc = (uint16_t) (nc * 2u);
    }

    const char **exports =
        (const char **) xr_realloc(m->export_names, (size_t) nc * sizeof(const char *));
    const char **owned =
        (const char **) xr_realloc(m->owned_names, (size_t) nc * sizeof(const char *));
    uint8_t *consts = (uint8_t *) xr_realloc(m->owned_consts, (size_t) nc * sizeof(uint8_t));
    XiConstLiteral *lits =
        (XiConstLiteral *) xr_realloc(m->const_literals, (size_t) nc * sizeof(XiConstLiteral));
    XiConstLiteral *shared_inits =
        (XiConstLiteral *) xr_realloc(m->shared_initializers, (size_t) nc * sizeof(XiConstLiteral));
    if (!exports || !owned || !consts || !lits || !shared_inits)
        return false;

    uint16_t old = m->cap;
    m->export_names = exports;
    m->owned_names = owned;
    m->owned_consts = consts;
    m->const_literals = lits;
    m->shared_initializers = shared_inits;
    memset(&m->export_names[old], 0, (size_t) (nc - old) * sizeof(const char *));
    memset(&m->owned_names[old], 0, (size_t) (nc - old) * sizeof(const char *));
    memset(&m->owned_consts[old], 0, (size_t) (nc - old) * sizeof(uint8_t));
    memset(&m->const_literals[old], 0, (size_t) (nc - old) * sizeof(XiConstLiteral));
    memset(&m->shared_initializers[old], 0, (size_t) (nc - old) * sizeof(XiConstLiteral));
    m->cap = nc;
    return true;
}

static void prescan_slot_meta_free(PrescanSlotMeta *m) {
    if (!m)
        return;
    xr_free(m->export_names);
    xr_free(m->owned_names);
    xr_free(m->owned_consts);
    xr_free(m->const_literals);
    xr_free(m->shared_initializers);
    memset(m, 0, sizeof(*m));
}

static bool const_literal_from_ast(XiLower *l, AstNode *expr, struct XrType *type,
                                   XiConstLiteral *out) {
    if (!l || !expr || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->type = type;
    switch (expr->type) {
        case AST_LITERAL_INT:
            out->kind = XI_CONST_LITERAL_INT;
            out->type = type ? type : l->type_int;
            out->int_value = expr->as.literal.raw_value.int_val;
            return true;
        case AST_LITERAL_FLOAT:
            out->kind = XI_CONST_LITERAL_FLOAT;
            out->type = type ? type : l->type_float;
            out->float_value = expr->as.literal.raw_value.float_val;
            return true;
        case AST_UNARY_NEG: {
            AstNode *operand = expr->as.unary.operand;
            if (!operand)
                return false;
            if (operand->type == AST_LITERAL_INT) {
                out->kind = XI_CONST_LITERAL_INT;
                out->type = type ? type : l->type_int;
                out->int_value = (int64_t) (0u - (uint64_t) operand->as.literal.raw_value.int_val);
                return true;
            }
            if (operand->type == AST_LITERAL_FLOAT) {
                out->kind = XI_CONST_LITERAL_FLOAT;
                out->type = type ? type : l->type_float;
                out->float_value = -operand->as.literal.raw_value.float_val;
                return true;
            }
            return false;
        }
        case AST_LITERAL_TRUE:
            out->kind = XI_CONST_LITERAL_BOOL;
            out->type = type ? type : l->type_bool;
            out->bool_value = true;
            return true;
        case AST_LITERAL_FALSE:
            out->kind = XI_CONST_LITERAL_BOOL;
            out->type = type ? type : l->type_bool;
            out->bool_value = false;
            return true;
        case AST_LITERAL_RUNE:
            out->kind = XI_CONST_LITERAL_CHAR;
            out->type = type ? type : l->type_rune;
            out->int_value = (int64_t) expr->as.literal.raw_value.rune_val;
            return true;
        case AST_LITERAL_STRING:
            out->kind = XI_CONST_LITERAL_STRING;
            out->type = type ? type : l->type_string;
            out->string_value = arena_strdup(l->func, expr->as.literal.raw_value.string_val);
            return out->string_value != NULL;
        case AST_LITERAL_NULL:
            out->kind = XI_CONST_LITERAL_NULL;
            out->type = type ? type : l->type_null;
            return true;
        default:
            return false;
    }
}

static bool const_literal_from_ct_value(XiLower *l, const XrCtValue *value, struct XrType *type,
                                        XiConstLiteral *out);

static void const_literal_normalize_for_static_slot_type(XiConstLiteral *lit, struct XrType *type) {
    if (!lit || !type)
        return;
    if (type->kind == XR_KIND_FLOAT && lit->kind == XI_CONST_LITERAL_INT) {
        lit->kind = XI_CONST_LITERAL_FLOAT;
        lit->float_value = (double) lit->int_value;
    }
    lit->type = type;
}

static bool shared_static_initializer_from_decl(XiLower *l, AstNode *s, struct XrType *type,
                                                XiConstLiteral *out) {
    if (!l || !s || (s->type != AST_SHARED_DECL && s->type != AST_VAR_DECL) ||
        !s->as.var_decl.initializer || !out)
        return false;
    XiConstLiteral lit;
    if (!const_literal_from_ast(l, s->as.var_decl.initializer, type, &lit)) {
        XaSymbol *sym =
            (l->analyzer && s->as.var_decl.symbol_id != 0)
                ? xa_scope_lookup_by_id(l->analyzer->global_scope, s->as.var_decl.symbol_id)
                : NULL;
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(l->analyzer, sym) : NULL;
        if (!links || !links->has_ct_value ||
            !const_literal_from_ct_value(l, &links->ct_value, type, &lit)) {
            XrCtValue value = {0};
            const char *err = NULL;
            (void) err;
            if (!l->analyzer ||
                !xa_consteval_expr(l->analyzer, s->as.var_decl.initializer, &value, &err) ||
                !const_literal_from_ct_value(l, &value, type, &lit))
                return false;
        }
    }
    const_literal_normalize_for_static_slot_type(&lit, type);
    if (s->type == AST_VAR_DECL) {
        lit.data_mutable = true;
        prescan_apply_static_data_attrs(l, &s->as.var_decl, &lit);
    }
    switch (lit.kind) {
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_FLOAT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_STRING:
        case XI_CONST_LITERAL_NULL:
        case XI_CONST_LITERAL_COMPTIME_AGGREGATE:
            *out = lit;
            return true;
        default:
            return false;
    }
}

static bool shared_default_initializer_from_type(struct XrType *type, XiConstLiteral *out) {
    if (!type || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->type = type;
    if (type->is_nullable) {
        out->kind = XI_CONST_LITERAL_NULL;
        return true;
    }
    switch (type->kind) {
        case XR_KIND_INT:
            out->kind = XI_CONST_LITERAL_INT;
            out->int_value = 0;
            return true;
        case XR_KIND_FLOAT:
            out->kind = XI_CONST_LITERAL_FLOAT;
            out->float_value = 0.0;
            return true;
        case XR_KIND_BOOL:
            out->kind = XI_CONST_LITERAL_BOOL;
            out->bool_value = false;
            return true;
        case XR_KIND_NULL:
            out->kind = XI_CONST_LITERAL_NULL;
            return true;
        default:
            return false;
    }
}

static XrCtValue *copy_ct_value_to_func(XiFunc *func, const XrCtValue *src) {
    if (!func || !src)
        return NULL;
    XrCtValue *dst = (XrCtValue *) xi_func_arena_alloc(func, (uint32_t) sizeof(XrCtValue));
    if (!dst)
        return NULL;
    *dst = *src;
    switch (src->kind) {
        case XR_CT_STRING:
            dst->as.string_val = arena_strdup(func, src->as.string_val);
            if (src->as.string_val && !dst->as.string_val)
                return NULL;
            break;
        case XR_CT_FIXED_ARRAY: {
            int count = src->as.fixed_array_val.count;
            dst->as.fixed_array_val.elements = NULL;
            if (count > 0) {
                XrCtValue *elements = (XrCtValue *) xi_func_arena_alloc(
                    func, (uint32_t) ((size_t) count * sizeof(XrCtValue)));
                if (!elements)
                    return NULL;
                dst->as.fixed_array_val.elements = elements;
                for (int i = 0; i < count; i++) {
                    XrCtValue *copy =
                        copy_ct_value_to_func(func, &src->as.fixed_array_val.elements[i]);
                    if (!copy)
                        return NULL;
                    elements[i] = *copy;
                }
            }
            break;
        }
        case XR_CT_TUPLE: {
            int count = src->as.tuple_val.count;
            dst->as.tuple_val.elements = NULL;
            if (count > 0) {
                XrCtValue *elements = (XrCtValue *) xi_func_arena_alloc(
                    func, (uint32_t) ((size_t) count * sizeof(XrCtValue)));
                if (!elements)
                    return NULL;
                dst->as.tuple_val.elements = elements;
                for (int i = 0; i < count; i++) {
                    XrCtValue *copy = copy_ct_value_to_func(func, &src->as.tuple_val.elements[i]);
                    if (!copy)
                        return NULL;
                    elements[i] = *copy;
                }
            }
            break;
        }
        case XR_CT_STRUCT_VALUE: {
            int count = src->as.struct_val.field_count;
            dst->as.struct_val.struct_name = arena_strdup(func, src->as.struct_val.struct_name);
            dst->as.struct_val.field_names = NULL;
            dst->as.struct_val.field_values = NULL;
            if (src->as.struct_val.struct_name && !dst->as.struct_val.struct_name)
                return NULL;
            if (count > 0) {
                const char **names = (const char **) xi_func_arena_alloc(
                    func, (uint32_t) ((size_t) count * sizeof(const char *)));
                XrCtValue *values = (XrCtValue *) xi_func_arena_alloc(
                    func, (uint32_t) ((size_t) count * sizeof(XrCtValue)));
                if (!names || !values)
                    return NULL;
                dst->as.struct_val.field_names = names;
                dst->as.struct_val.field_values = values;
                for (int i = 0; i < count; i++) {
                    names[i] = arena_strdup(func, src->as.struct_val.field_names
                                                      ? src->as.struct_val.field_names[i]
                                                      : NULL);
                    if (src->as.struct_val.field_names && src->as.struct_val.field_names[i] &&
                        !names[i])
                        return NULL;
                    XrCtValue *copy =
                        copy_ct_value_to_func(func, &src->as.struct_val.field_values[i]);
                    if (!copy)
                        return NULL;
                    values[i] = *copy;
                }
            }
            break;
        }
        case XR_CT_NONE:
            return NULL;
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
        case XR_CT_NULL:
            break;
    }
    return dst;
}

static bool const_literal_from_ct_value(XiLower *l, const XrCtValue *value, struct XrType *type,
                                        XiConstLiteral *out) {
    if (!l || !value || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->type = type;
    switch (value->kind) {
        case XR_CT_INT:
            out->kind = XI_CONST_LITERAL_INT;
            out->type = type ? type : l->type_int;
            out->int_value = value->as.int_val;
            return true;
        case XR_CT_FLOAT:
            out->kind = XI_CONST_LITERAL_FLOAT;
            out->type = type ? type : l->type_float;
            out->float_value = value->as.float_val;
            return true;
        case XR_CT_BOOL:
            out->kind = XI_CONST_LITERAL_BOOL;
            out->type = type ? type : l->type_bool;
            out->bool_value = value->as.bool_val;
            return true;
        case XR_CT_CHAR:
            out->kind = XI_CONST_LITERAL_CHAR;
            out->type = type ? type : l->type_rune;
            out->int_value = (int64_t) value->as.rune_val;
            return true;
        case XR_CT_STRING:
            if (!value->as.string_val)
                return false;
            out->kind = XI_CONST_LITERAL_STRING;
            out->type = type ? type : l->type_string;
            out->string_value = arena_strdup(l->func, value->as.string_val);
            return out->string_value != NULL;
        case XR_CT_NULL:
            out->kind = XI_CONST_LITERAL_NULL;
            out->type = type ? type : l->type_null;
            return true;
        case XR_CT_FIXED_ARRAY:
        case XR_CT_TUPLE:
        case XR_CT_STRUCT_VALUE:
            out->ct_value = copy_ct_value_to_func(l->func, value);
            if (!out->ct_value)
                return false;
            out->kind = XI_CONST_LITERAL_COMPTIME_AGGREGATE;
            out->type = type;
            return true;
        default:
            return false;
    }
}

XR_FUNC const char *xi_lower_enum_method_hidden_name(XiFunc *arena, const char *enum_name,
                                                     const char *method_name, bool is_static) {
    if (!arena || !enum_name || !method_name)
        return NULL;
    const char *kind = is_static ? "static" : "inst";
    int needed = snprintf(NULL, 0, "__xray_enum_method$%s$%s$%s", enum_name, kind, method_name);
    if (needed < 0)
        return NULL;
    char *buf = (char *) xi_func_arena_alloc(arena, (uint32_t) needed + 1u);
    if (!buf)
        return NULL;
    snprintf(buf, (size_t) needed + 1u, "__xray_enum_method$%s$%s$%s", enum_name, kind,
             method_name);
    return buf;
}

static void prescan_enum_method_bindings(XiLower *l, EnumDeclNode *ed, uint16_t *next_shared) {
    if (!l || !ed || !next_shared || !ed->name || ed->method_count <= 0)
        return;
    XaSymbol *enum_sym = xa_analyzer_lookup(l->analyzer, ed->name);
    XaSymbolLinks *enum_links = enum_sym ? xa_analyzer_get_links(l->analyzer, enum_sym) : NULL;
    XrClassInfo *info = enum_links ? enum_links->class_info : NULL;
    if (!info)
        return;

    for (int i = 0; i < ed->method_count; i++) {
        AstNode *method = ed->methods ? ed->methods[i] : NULL;
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        XaSymbol *method_sym = xa_class_info_lookup_member(info, md->name);
        if (!method_sym || method_sym->kind != XA_SYM_METHOD ||
            method_sym->is_static != md->is_static)
            continue;
        XaSymbolLinks *method_links = xa_analyzer_get_links(l->analyzer, method_sym);
        const char *hidden =
            xi_lower_enum_method_hidden_name(l->func, ed->name, md->name, md->is_static);
        if (!hidden)
            continue;
        int vid = xi_lower_var_create(l, method_sym->id, hidden,
                                      method_links ? method_links->type : l->type_any);
        XR_DCHECK(vid >= 0 && vid < l->var_cap, "prescan_enum_method_bindings: var_id overflow");
        l->shared_map[vid] = (int16_t) *next_shared;
        (*next_shared)++;
    }
}

/*
 * Top-level binding prescan: allocate a shared slot for every top-level
 * declaration (var / const / fn / class / enum / struct / import member)
 * and record per-slot metadata used by REPL symbol collection and module
 * export emission.
 *
 * The same pass serves both REPL incremental compilation and script /
 * module compilation: the only mode-dependent piece is how the lowerer
 * eventually emits loads / stores against these slots (XI_GET/SET_GLOBAL
 * in REPL, XI_GET/SET_SHARED otherwise), and that decision belongs to
 * xi_lower_emit_top_load / _store — not here.  Allocating slots in REPL
 * mode is harmless (the emit helper ignores them when it picks
 * XI_GET_GLOBAL) and lets all downstream code treat top bindings
 * uniformly.
 *
 * Populates: l->shared_map[var_id], l->func->nshared,
 *            l->func->slot_owned_names, l->func->slot_owned_consts,
 *            l->func->export_names (for AST_EXPORT_STMT list form).
 */
static void prescan_top_level_bindings(XiLower *l, AstNode **stmts, int count,
                                       uint16_t start_shared) {
    XR_DCHECK(l->is_program, "prescan_top_level_bindings: not a program context");
    uint16_t next_shared = start_shared;

    PrescanSlotMeta slot_meta;
    memset(&slot_meta, 0, sizeof(slot_meta));
    if (!prescan_slot_meta_reserve(&slot_meta, start_shared > 0 ? start_shared : 16))
        return;

    for (int i = 0; i < count; i++) {
        AstNode *s = stmts[i];
        if (!s)
            continue;

        const char *name = NULL;
        uint32_t sid = 0;
        struct XrType *type = NULL;
        bool is_const = false;
        bool is_exported = false;
        s = prescan_extract_decl(l, s, &name, &sid, &type, &is_const, &is_exported);

        /* Export-list form: export a, b, c */
        if (s && s->type == AST_EXPORT_STMT && s->as.export_stmt.export_names) {
            ExportStmtNode *exp = &s->as.export_stmt;
            for (int ei = 0; ei < exp->export_count; ei++) {
                const char *ename = exp->export_names[ei];
                if (!ename)
                    continue;
                int vid = xi_lower_var_find(l, 0, ename);
                if (vid >= 0 && l->shared_map[vid] >= 0) {
                    int slot = l->shared_map[vid];
                    if (slot >= 0 && prescan_slot_meta_reserve(&slot_meta, (uint16_t) slot + 1u))
                        slot_meta.export_names[slot] = ename;
                }
            }
            continue;
        }

        /* Handle imports */
        if (s && s->type == AST_IMPORT_STMT) {
            if (s->as.import_stmt.member_count == 0) {
                name = s->as.import_stmt.alias ? s->as.import_stmt.alias
                                               : s->as.import_stmt.module_name;
                sid = s->as.import_stmt.symbol_id;
            } else {
                for (int mi = 0; mi < s->as.import_stmt.member_count; mi++) {
                    ImportMember *m = &s->as.import_stmt.members[mi];
                    const char *mname = m->alias ? m->alias : m->name;
                    if (!mname)
                        continue;
                    int vid = xi_lower_var_create(l, m->symbol_id, mname, l->type_any);
                    XR_DCHECK(vid >= 0 && vid < l->var_cap,
                              "prescan_top_level_bindings: var_id overflow (import member)");
                    l->shared_map[vid] = (int16_t) next_shared;
                    next_shared++;
                }
                continue;
            }
        }
        if (!name)
            continue;

        int var_id = xi_lower_var_create(l, sid, name, type);
        XR_DCHECK(var_id >= 0 && var_id < l->var_cap,
                  "prescan_top_level_bindings: var_id overflow");
        l->shared_map[var_id] = (int16_t) next_shared;
        if (!prescan_slot_meta_reserve(&slot_meta, next_shared + 1u)) {
            prescan_slot_meta_free(&slot_meta);
            return;
        }
        if (prescan_is_user_owned_decl(s)) {
            slot_meta.owned_names[next_shared] = name;
            slot_meta.owned_consts[next_shared] = is_const ? 1u : 0u;
        }
        if (is_const && s && s->type == AST_CONST_DECL && s->as.var_decl.initializer) {
            XiConstLiteral lit;
            XaSymbol *sym = (l->analyzer && sid != 0)
                                ? xa_scope_lookup_by_id(l->analyzer->global_scope, sid)
                                : NULL;
            XaSymbolLinks *links = sym ? xa_analyzer_get_links(l->analyzer, sym) : NULL;
            if (const_literal_from_ast(l, s->as.var_decl.initializer, type, &lit)) {
                prescan_apply_static_data_attrs(l, &s->as.var_decl, &lit);
                slot_meta.const_literals[next_shared] = lit;
            } else if (links && links->has_ct_value &&
                       const_literal_from_ct_value(l, &links->ct_value, type, &lit)) {
                prescan_apply_static_data_attrs(l, &s->as.var_decl, &lit);
                slot_meta.const_literals[next_shared] = lit;
            }
        } else if (s && (s->type == AST_SHARED_DECL || s->type == AST_VAR_DECL) &&
                   s->as.var_decl.initializer) {
            XiConstLiteral lit;
            if (shared_static_initializer_from_decl(l, s, type, &lit))
                slot_meta.shared_initializers[next_shared] = lit;
        } else if (s && s->type == AST_VAR_DECL && !s->as.var_decl.initializer) {
            XiConstLiteral lit;
            if (shared_default_initializer_from_type(type, &lit)) {
                lit.data_mutable = true;
                slot_meta.shared_initializers[next_shared] = lit;
            }
        }
        if (is_exported)
            slot_meta.export_names[next_shared] = name;
        next_shared++;

        if (s && s->type == AST_ENUM_DECL)
            prescan_enum_method_bindings(l, &s->as.enum_decl, &next_shared);
    }
    l->func->nshared = next_shared;

    /* Populate export_names, slot_owned_names, and optimization-time
     * shared-slot metadata on XiFunc. */
    if (next_shared > 0) {
        const char **names = (const char **) xi_func_arena_alloc(
            l->func, (uint32_t) (next_shared * sizeof(const char *)));
        const char **owned = (const char **) xi_func_arena_alloc(
            l->func, (uint32_t) (next_shared * sizeof(const char *)));
        uint8_t *consts =
            (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) (next_shared * sizeof(uint8_t)));
        XiConstLiteral *literals = (XiConstLiteral *) xi_func_arena_alloc(
            l->func, (uint32_t) (next_shared * sizeof(XiConstLiteral)));
        XiConstLiteral *shared_inits = (XiConstLiteral *) xi_func_arena_alloc(
            l->func, (uint32_t) (next_shared * sizeof(XiConstLiteral)));
        XiFunc **slot_funcs =
            (XiFunc **) xi_func_arena_alloc(l->func, (uint32_t) (next_shared * sizeof(XiFunc *)));
        l->func->shared_slot_funcs = slot_funcs;
        l->func->shared_slot_func_count = next_shared;
        l->func->shared_const_literals = literals;
        l->func->shared_const_literal_count = literals ? next_shared : 0;
        l->func->shared_init_literals = shared_inits;
        l->func->shared_init_literal_count = shared_inits ? next_shared : 0;
        for (uint16_t si = 0; si < next_shared; si++) {
            if (names) {
                const char *src = (si < slot_meta.cap) ? slot_meta.export_names[si] : NULL;
                if (src) {
                    uint32_t slen = (uint32_t) strlen(src);
                    char *copy = (char *) xi_func_arena_alloc(l->func, slen + 1);
                    if (copy)
                        memcpy(copy, src, slen + 1);
                    names[si] = copy;
                } else {
                    names[si] = NULL;
                }
            }
            if (owned) {
                const char *src = (si < slot_meta.cap) ? slot_meta.owned_names[si] : NULL;
                if (src) {
                    uint32_t slen = (uint32_t) strlen(src);
                    char *copy = (char *) xi_func_arena_alloc(l->func, slen + 1);
                    if (copy)
                        memcpy(copy, src, slen + 1);
                    owned[si] = copy;
                } else {
                    owned[si] = NULL;
                }
            }
            if (slot_funcs)
                slot_funcs[si] = NULL;
            if (literals) {
                if (si < slot_meta.cap)
                    literals[si] = slot_meta.const_literals[si];
                else
                    memset(&literals[si], 0, sizeof(XiConstLiteral));
            }
            if (shared_inits) {
                if (si < slot_meta.cap)
                    shared_inits[si] = slot_meta.shared_initializers[si];
                else
                    memset(&shared_inits[si], 0, sizeof(XiConstLiteral));
            }
        }
        if (consts) {
            for (uint16_t si = 0; si < next_shared; si++)
                consts[si] = (si < slot_meta.cap) ? slot_meta.owned_consts[si] : 0u;
        }
        if (names)
            l->func->export_names = names;
        if (owned)
            l->func->slot_owned_names = owned;
        if (consts)
            l->func->slot_owned_consts = consts;
    }
    prescan_slot_meta_free(&slot_meta);
}

/*
 * Recursively decorate capture metadata on the function tree.
 * Sets capture_kind and is_mutable based on the already-computed needs_cell
 * flag from the lowering-time closure analysis.  This finalizes the metadata
 * so downstream passes (emit, AOT) can read XiCapture.capture_kind
 * instead of interpreting needs_cell + source heuristically.
 */
static void finalize_capture_metadata(XiFunc *f) {
    XR_DCHECK(f != NULL, "finalize_capture_metadata: NULL func");

    for (uint16_t i = 0; i < f->ncaptures; i++) {
        XiCapture *cap = &f->captures[i];
        if (cap->needs_cell) {
            cap->capture_kind = (uint8_t) XI_CAPTURE_BY_MUT_CELL;
            cap->is_mutable = true;
        } else {
            cap->capture_kind = (uint8_t) XI_CAPTURE_BY_COPY;
            cap->is_mutable = false;
        }
    }

    /* Recurse into child functions */
    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (f->children[ci])
            finalize_capture_metadata(f->children[ci]);
    }
}

/*
 * Build XiModule metadata directly from lowerer tracking data.
 * Constructs the exports table from export_names + shared_slot metadata
 * without scanning IR instructions.  Also collects class data into
 * module->classes for AOT codegen.
 */
static void build_module_metadata(XiLower *l) {
    XiFunc *f = l->func;
    XR_DCHECK(f != NULL, "build_module_metadata: NULL func");

    /* Allocate module (caller must free via xi_module_free) */
    XiModule *mod = xi_module_new(NULL, NULL, f);
    if (!mod)
        return;

    if (l->global_asm_count > 0) {
        mod->global_asm_templates =
            (const char **) xr_calloc((size_t) l->global_asm_count, sizeof(const char *));
        XR_CHECK(mod->global_asm_templates != NULL, "xi_lower: global asm metadata OOM");
        for (int i = 0; i < l->global_asm_count; i++)
            mod->global_asm_templates[i] = l->global_asm_templates[i];
        mod->nglobal_asm = (uint16_t) l->global_asm_count;
    }

    uint16_t nshared = f->nshared;

    /* Build exports from export_names + tracked function/class pointers */
    if (f->export_names && nshared > 0) {
        uint16_t nexports = 0;
        for (uint16_t s = 0; s < nshared; s++) {
            if (f->export_names[s])
                nexports++;
        }
        if (nexports > 0) {
            XiModuleExport *exps = (XiModuleExport *) xr_calloc(nexports, sizeof(XiModuleExport));
            if (exps) {
                uint16_t ei = 0;
                for (uint16_t s = 0; s < nshared && ei < nexports; s++) {
                    if (!f->export_names[s])
                        continue;
                    exps[ei].name = f->export_names[s];
                    exps[ei].shared_slot = s;
                    exps[ei].cell_index = -1;
                    exps[ei].function = l->shared_slot_funcs[s];
                    exps[ei].class_data = l->shared_slot_classes[s];
                    /* Type info from the var entry that maps to this slot */
                    for (int vi = 0; vi < l->var_count; vi++) {
                        if (l->shared_map[vi] == (int16_t) s) {
                            exps[ei].value_type = l->vars[vi].type;
                            break;
                        }
                    }
                    exps[ei].is_live_binding = false;
                    ei++;
                }
                mod->exports = exps;
                mod->nexports = nexports;
            }
        }
    }

    /* Collect class data from tracked slots */
    uint16_t class_count = 0;
    for (uint16_t s = 0; s < nshared; s++) {
        if (l->shared_slot_classes[s])
            class_count++;
    }
    if (class_count > 0) {
        XiClassData **cls = (XiClassData **) xr_calloc(class_count, sizeof(XiClassData *));
        if (cls) {
            uint16_t ci = 0;
            for (uint16_t s = 0; s < nshared && ci < class_count; s++) {
                if (l->shared_slot_classes[s])
                    cls[ci++] = l->shared_slot_classes[s];
            }
            mod->classes = cls;
            mod->nclasses = class_count;
        }
    }

    /* Shared-slot → function/class mappings for C codegen.
     * These parallel arrays let cgen resolve GET_SHARED(slot) without
     * scanning IR blocks for SET_SHARED patterns. */
    if (nshared > 0) {
        mod->nslots = nshared;
        mod->slot_funcs = (XiFunc **) xr_calloc(nshared, sizeof(XiFunc *));
        mod->slot_classes = (XiClassData **) xr_calloc(nshared, sizeof(XiClassData *));
        mod->slot_enums = (XiEnumData **) xr_calloc(nshared, sizeof(XiEnumData *));
        mod->slot_imports = (XiImportRef **) xr_calloc(nshared, sizeof(XiImportRef *));
        mod->slot_const_literals = (XiConstLiteral *) xr_calloc(nshared, sizeof(XiConstLiteral));
        mod->slot_shared_initializers =
            (XiConstLiteral *) xr_calloc(nshared, sizeof(XiConstLiteral));
        if (mod->slot_funcs && mod->slot_classes && mod->slot_enums && mod->slot_imports &&
            mod->slot_const_literals && mod->slot_shared_initializers) {
            for (uint16_t s = 0; s < nshared; s++) {
                mod->slot_funcs[s] = l->shared_slot_funcs[s];
                mod->slot_classes[s] = l->shared_slot_classes[s];
                mod->slot_enums[s] = l->shared_slot_enums[s];
                mod->slot_imports[s] = l->shared_slot_imports[s];
                if (f->shared_const_literals && s < f->shared_const_literal_count)
                    mod->slot_const_literals[s] = f->shared_const_literals[s];
                if (f->shared_init_literals && s < f->shared_init_literal_count)
                    mod->slot_shared_initializers[s] = f->shared_init_literals[s];
            }
        }
    }

    /* xi_module_new already copies init->children into mod->functions */
    f->module = mod;
}

XR_FUNC XiFunc *xi_lower_program(AstNode *program_node, struct XaAnalyzer *analyzer,
                                 struct XrVMRuntime *isolate) {
    return xi_lower_program_ex(program_node, analyzer, isolate, false, NULL, 0);
}

XR_FUNC XiFunc *xi_lower_program_ex(AstNode *program_node, struct XaAnalyzer *analyzer,
                                    struct XrVMRuntime *isolate, bool repl_mode,
                                    const struct XgGlobalEvidence *global_evidence,
                                    uint32_t module_id) {
    XR_CHECK(program_node != NULL, "xi_lower_program: node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_program: analyzer is NULL");

    XiLower l;
    xi_lower_init(&l, analyzer, isolate);
    l.is_program = true;
    l.repl_mode = repl_mode;
    l.global_evidence = global_evidence;
    l.xg_module_id = module_id;

    l.func = xi_func_new("<main>", l.type_unit);
    if (!l.func) {
        xi_lower_cleanup(&l);
        return NULL;
    }
    l.func->analyzer = analyzer;
    l.func->nparams = 0;
    l.func->params = NULL;
    xi_lower_bind_module_body_id(&l);

    XiBlock *entry = xi_block_new(l.func);
    entry->sealed = true;
    l.cur_block = entry;

    /* REPL seed: register prior-input top-level declarations so the
     * current input's references resolve by name.  Each seeded name
     * gets a sentinel shared_map entry (>= 0) so the lowerer knows
     * it is a top-level variable and emits OP_GETGLOBAL/OP_SETGLOBAL.
     * A session-owned repl_symbols table exists only in REPL mode, so
     * this is a no-op for ordinary script compilation. */
    uint16_t next_shared_start = 0;
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrReplSymbolTable *repl_syms = xr_compiler_session_repl_symbols(session);
    if (repl_syms) {
        for (int i = 0; i < repl_syms->count; i++) {
            XrReplSymbol *s = &repl_syms->symbols[i];
            if (!s->name || s->name->length == 0)
                continue;
            int vid = xi_lower_var_create(&l, 0, s->name->data, l.type_any);
            if (vid < 0 || vid >= l.var_cap)
                continue;
            l.shared_map[vid] = (int16_t) i;
            if (i + 1 > (int) next_shared_start)
                next_shared_start = (uint16_t) (i + 1);
        }
    }

    /* Pre-scan: register every top-level declaration and assign it a
     * shared slot.  REPL and module/script modes share this pass; the
     * mode only affects how loads / stores are emitted later, never
     * how slots are allocated. */
    AstNode **stmts;
    int count;
    if (program_node->type == AST_BLOCK) {
        stmts = program_node->as.block.statements;
        count = program_node->as.block.count;
    } else {
        stmts = program_node->as.program.statements;
        count = program_node->as.program.count;
    }
    prescan_top_level_bindings(&l, stmts, count, next_shared_start);

    xi_lower_defer_scope_push(&l);

    /* Lower top-level declaration values before executable code so forward
     * references see initialized bindings, not shared-slot null values. */
    for (int i = 0; i < count; i++) {
        if (!l.cur_block)
            break;
        AstNode *s = stmts[i];
        if (!s)
            continue;
        AstNode *decl = (s->type == AST_EXPORT_STMT) ? s->as.export_stmt.declaration : s;
        if (decl && (decl->type == AST_FUNCTION_DECL || decl->type == AST_CLASS_DECL ||
                     decl->type == AST_STRUCT_DECL || decl->type == AST_UNION_DECL ||
                     decl->type == AST_ENUM_DECL)) {
            xi_lower_stmt(&l, decl);
        }
    }

    /* Lower remaining top-level statements in source order */
    for (int i = 0; i < count; i++) {
        if (!l.cur_block)
            break;
        AstNode *s = stmts[i];
        if (!s)
            continue;
        AstNode *decl = (s->type == AST_EXPORT_STMT) ? s->as.export_stmt.declaration : s;
        if (decl && (decl->type == AST_FUNCTION_DECL || decl->type == AST_CLASS_DECL ||
                     decl->type == AST_STRUCT_DECL || decl->type == AST_UNION_DECL ||
                     decl->type == AST_ENUM_DECL))
            continue; /* already hoisted above */
        xi_lower_stmt(&l, s);
    }

    xi_lower_defer_scope_pop_normal(&l, program_node->line);

    if (l.cur_block) {
        xi_block_set_return(l.cur_block, NULL);
    }

    /* Build module metadata from lowerer tracking data (no IR scan needed) */
    if (!l.had_error) {
        /* Rewrite generator calls before metadata/effects: every function is now
         * lowered, so generator callees are reliably identifiable (handles
         * forward/nested references), and this runs before escape analysis. */
        xi_lower_rewrite_generator_calls(l.func);
        build_module_metadata(&l);
        finalize_capture_metadata(l.func);
        xi_func_compute_effects(l.func);
    }

    XiFunc *result = NULL;
    if (!l.had_error && xi_lower_capture_source_vars(&l)) {
        result = l.func;
        result->stage = XI_STAGE_RAW;
        result->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);
        xi_lower_assert_var_ids(&l, result);
    }
    xi_lower_cleanup(&l);
    return result;
}
