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
#include "../frontend/lexer/xlex.h"

#include "../runtime/class/xenum.h"
#include "../runtime/object/xstring.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/xisolate_api.h"

#include <string.h>
#include <stdio.h>

/* Forward declarations */
static void finalize_capture_metadata(XiFunc *f);

/* ========== Braun SSA: Variable Management ========== */

/* Register a variable by its analyzer-assigned symbol_id.
 * Each unique symbol_id gets exactly one var_id slot.  If the same
 * symbol_id is registered again (e.g. redeclaration in the same scope),
 * the existing var_id is reused.  Different symbol_ids with the same
 * name (shadows) naturally get distinct var_ids because the analyzer
 * assigned them different IDs during scope resolution. */
XR_FUNC int xi_lower_var_create(XiLower *l, uint32_t symbol_id,
                                 const char *name, struct XrType *type) {
    XR_DCHECK(name != NULL, "var_create: name is NULL");

    /* If symbol_id is resolved (non-zero), look up by ID — O(n) but n < 256. */
    if (symbol_id != 0) {
        for (int i = 0; i < l->var_count; i++) {
            if (l->vars[i].symbol_id == symbol_id)
                return i;
        }
    } else {
        /* Fallback for synthetic variables (no analyzer symbol): match by name. */
        for (int i = l->var_count - 1; i >= 0; i--) {
            if (l->vars[i].symbol_id == 0 &&
                l->vars[i].name && strcmp(l->vars[i].name, name) == 0)
                return i;
        }
    }

    XR_CHECK(l->var_count < XI_LOWER_MAX_VARS, "xi_lower: too many variables");
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


/* ========== Shared Variable Lookup ========== */

/* Walk the parent chain to find a program-level shared variable.
 * Returns the shared index (>=0) if found, or -1 if not.
 * Sets *out_type to the variable type when found. */
XR_FUNC int xi_lower_find_shared(XiLower *l, uint32_t symbol_id,
                                  const char *name, struct XrType **out_type) {
    for (XiLower *p = l->parent; p; p = p->parent) {
        if (!p->is_program) continue;
        int var_id = xi_lower_var_find(p, symbol_id, name);
        if (var_id >= 0 && p->shared_map[var_id] >= 0) {
            if (out_type) *out_type = p->vars[var_id].type;
            return p->shared_map[var_id];
        }
    }
    return -1;
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
XR_FUNC int xi_lower_resolve_upvalue(XiLower *l, uint32_t symbol_id,
                                      const char *name, struct XrType **out_type) {
    XiLower *parent = l->parent;
    if (!parent) return -1;

    /* Program-level shared variables are handled via XI_GET_SHARED
     * in lower_variable/lower_assignment, not via upvalue capture. */
    if (parent->is_program) return -1;

    /* Dedup: if this variable is already captured, return existing index */
    for (uint16_t ci = 0; ci < l->func->ncaptures; ci++) {
        if (l->func->captures[ci].name &&
            strcmp(l->func->captures[ci].name, name) == 0) {
            if (out_type) *out_type = l->func->captures[ci].type;
            return (int)ci;
        }
    }

    /* Check if the variable exists as a local in the immediate parent */
    int var_id = xi_lower_var_find(parent, symbol_id, name);
    if (var_id >= 0) {
        /* Read the current SSA value from the parent's scope.  The value's
         * register will be resolved at emit time via reg_of(). */
        XiValue *parent_val = xi_lower_braun_read(parent, var_id, parent->cur_block);
        if (l->func->ncaptures >= XI_MAX_CAPTURES) return -1;
        int idx = l->func->ncaptures;
        l->func->captures[idx].source = XI_CAPTURE_SRC_REG;
        l->func->captures[idx].index = 0;
        l->func->captures[idx].name = name;
        l->func->captures[idx].type = parent->vars[var_id].type;
        l->func->captures[idx].value = parent_val;
        /* Cell indirection is needed when the capture cannot see the final
         * value at closure creation time:
         *  - Hoisted function variables: initially null, replaced by the
         *    actual closure later.
         *  - Variables captured during function hoisting that have no real
         *    definition yet (braun_read returned a null placeholder): the
         *    actual initializer runs after the closure is created. */
        bool forward_ref = (parent_val && parent_val->op == XI_CONST &&
                            parent_val->type == parent->type_null);
        l->func->captures[idx].needs_cell =
            parent->vars[var_id].hoisted || forward_ref;
        l->func->ncaptures++;
        if (out_type) *out_type = parent->vars[var_id].type;
        return idx;
    }

    /* Not a local in parent — try grandparent (transitive capture) */
    int parent_upval = xi_lower_resolve_upvalue(parent, symbol_id, name, out_type);
    if (parent_upval >= 0) {
        if (l->func->ncaptures >= XI_MAX_CAPTURES) return -1;
        int idx = l->func->ncaptures;
        l->func->captures[idx].source = XI_CAPTURE_SRC_UPVAL;
        l->func->captures[idx].index = (uint8_t)parent_upval;
        l->func->captures[idx].name = name;
        l->func->captures[idx].type = out_type ? *out_type : l->type_any;
        /* Inherit needs_cell from the parent capture so CELL_GET is emitted
         * at every level in the transitive capture chain. */
        if (parent_upval < (int)parent->func->ncaptures)
            l->func->captures[idx].needs_cell =
                parent->func->captures[parent_upval].needs_cell;
        l->func->ncaptures++;
        return idx;
    }

    return -1;
}

/* Write: currentDef[var][block] = value */
XR_FUNC void xi_lower_braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val) {
    XR_DCHECK(var_id >= 0 && var_id < XI_LOWER_MAX_VARS,
              "braun_write: var_id out of range");
    XR_DCHECK(blk->id < XI_LOWER_MAX_BLOCKS,
              "braun_write: block_id out of range");
    l->var_defs[var_id * XI_LOWER_MAX_BLOCKS + blk->id] = val;
    /* Tag value with source variable for register coalescing.
     * Skip if the value already belongs to a different variable:
     * overwriting would merge two unrelated variables onto one
     * physical register, corrupting phi operands at loop edges. */
    if (val && var_id >= 0 && var_id < 255) {
        if (val->var_id == 0xFF || val->var_id == (uint8_t)var_id)
            val->var_id = (uint8_t)var_id;
        /* Definitions of variables captured by hoisted children must survive
         * DCE: the emitter redirects them through CELL_SET at emit time. */
        if (var_id < l->var_count && l->vars[var_id].captured_by_child)
            val->flags |= XI_FLAG_SIDE_EFFECT;
    }
}

/* Read: get currentDef[var][block], may be NULL. */
static XiValue *braun_read_local(XiLower *l, int var_id, XiBlock *blk) {
    XR_DCHECK(var_id >= 0 && var_id < XI_LOWER_MAX_VARS,
              "braun_read_local: var_id out of range");
    if (blk->id >= XI_LOWER_MAX_BLOCKS) return NULL;
    return l->var_defs[var_id * XI_LOWER_MAX_BLOCKS + blk->id];
}

/* Forward declarations */
static XiValue *braun_read_recursive(XiLower *l, int var_id, XiBlock *blk);
static XiValue *add_phi_operands(XiLower *l, int var_id, XiPhi *phi);

XR_FUNC XiValue *xi_lower_braun_read(XiLower *l, int var_id, XiBlock *blk) {
    XiValue *val = braun_read_local(l, var_id, blk);
    if (val) return val;
    return braun_read_recursive(l, var_id, blk);
}

/* Try to remove trivial phi: if all operands are the same (or self),
 * replace with that single value. */
static XiValue *try_remove_trivial_phi(XiLower *l, int var_id, XiPhi *phi) {
    XiValue *same = NULL;
    XiValue *pv = &phi->value;

    for (uint16_t i = 0; i < pv->nargs; i++) {
        XiValue *op = pv->args[i];
        if (op == same || op == pv)
            continue;  /* self-reference or same as current candidate */
        if (same != NULL)
            return pv;  /* non-trivial: two distinct operands */
        same = op;
    }

    if (same == NULL)
        return pv;  /* undefined — keep the phi */

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
    if (!type) type = l->type_any;

    if (!blk->sealed) {
        /* Block not sealed: create an incomplete phi placeholder.
         * Operands will be filled in braun_seal_block(). */
        XiPhi *phi = xi_phi_new(l->func, blk, type, 0);
        phi->value.var_id = (var_id < 255) ? (uint8_t)var_id : 0xFF;
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
        if (val && var_id >= 0 && var_id < 255)
            val->var_id = (uint8_t)var_id;
    } else if (blk->npreds == 1) {
        /* Single predecessor: no phi needed, recurse. */
        val = xi_lower_braun_read(l, var_id, blk->preds[0]);
    } else {
        /* Multiple predecessors: insert phi, then fill operands. */
        XiPhi *phi = xi_phi_new(l->func, blk, type, blk->npreds);
        phi->value.var_id = (var_id < 255) ? (uint8_t)var_id : 0xFF;
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
        phi->value.args = (XiValue **) xi_func_arena_alloc(
            l->func, blk->npreds * sizeof(XiValue *));
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
    return t ? t : l->type_any;
}



/* ========== Context Initialization ========== */

XR_FUNC void xi_lower_init(XiLower *l, struct XaAnalyzer *analyzer,
                            struct XrayIsolate *isolate) {
    memset(l, 0, sizeof(XiLower));
    l->analyzer = analyzer;
    l->isolate = isolate;
    l->self_var_id = -1;

    /* Heap-allocate the 2D def map (256*256 pointers = 512KB) */
    size_t def_map_size = (size_t)XI_LOWER_MAX_VARS * XI_LOWER_MAX_BLOCKS;
    l->var_defs = (XiValue **) xr_calloc(def_map_size, sizeof(XiValue *));
    XR_CHECK(l->var_defs != NULL, "xi_lower: failed to allocate var_defs");

    /* Initialize shared_map to -1 (no shared index) */
    memset(l->shared_map, -1, sizeof(l->shared_map));

    /* Cache singleton types */
    l->type_int = xr_type_new_int(isolate);
    l->type_float = xr_type_new_float(isolate);
    l->type_bool = xr_type_new_bool(isolate);
    l->type_string = xr_type_new_string(isolate);
    l->type_null = xr_type_new_null(isolate);
    l->type_void = xr_type_new_void(isolate);
    l->type_any = xr_type_new_unknown(isolate);
    l->type_bigint = xr_type_new_bigint(isolate);
    l->type_regex = xr_type_new_regex(isolate);
}

XR_FUNC void xi_lower_cleanup(XiLower *l) {
    if (l->var_defs) {
        xr_free(l->var_defs);
        l->var_defs = NULL;
    }
}

/* ========== Method Symbol Resolution ========== */

XR_FUNC int32_t xi_lower_method_symbol(XiLower *l, const char *method_name) {
    if (!l->isolate || !method_name) return 0;
    XrSymbolTable *st = (XrSymbolTable *)xr_isolate_get_symbol_table(l->isolate);
    if (!st) return 0;
    return (int32_t)xr_symbol_register_in_table(st, method_name);
}

/* ========== Function Lowering Implementation ========== */

/*
 * Internal function lowering with optional parent context.
 * When parent is non-NULL, the child can resolve variable references
 * from enclosing scopes via the upvalue capture mechanism.
 */
XR_FUNC XiFunc *xi_lower_func_impl(AstNode *func_node, struct XaAnalyzer *analyzer,
                                     struct XrayIsolate *isolate, XiLower *parent_ctx) {
    XR_DCHECK(func_node != NULL, "lower_func_impl: func_node is NULL");
    FunctionDeclNode *fdecl = &func_node->as.function_decl;

    XiLower l;
    xi_lower_init(&l, analyzer, isolate);
    l.parent = parent_ctx;

    /* Determine return type */
    struct XrType *ret_type = fdecl->return_type;
    if (!ret_type) ret_type = l.type_void;

    l.func = xi_func_new(fdecl->name ? fdecl->name : "<anonymous>", ret_type);
    if (!l.func) { xi_lower_cleanup(&l); return NULL; }
    l.func->analyzer = analyzer;

    /* Entry block (no predecessors — seal immediately) */
    XiBlock *entry = xi_block_new(l.func);
    entry->sealed = true;
    l.cur_block = entry;

    /* Lower parameters */
    l.func->nparams = (uint16_t) fdecl->param_count;
    if (fdecl->param_count > 0) {
        l.func->params = (XiValue **) xr_calloc(
            fdecl->param_count, sizeof(XiValue *));
        if (!l.func->params) { xi_func_free(l.func); xi_lower_cleanup(&l); return NULL; }
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
        l.func->entry_type = 2;  /* XR_ENTRY_GENERATOR */
    } else if (fdecl->required_count <
               (has_rest ? fdecl->param_count - 1 : fdecl->param_count)) {
        l.func->entry_type = 1;  /* XR_ENTRY_DEFAULTS */
    } else {
        l.func->entry_type = 0;  /* XR_ENTRY_NORMAL */
    }

    /* nparams excludes rest param (VM packs varargs into the rest slot) */
    if (has_rest) {
        l.func->nparams = (uint16_t)(fdecl->param_count - 1);
    }

    for (int i = 0; i < fdecl->param_count; i++) {
        XrParamNode *p = fdecl->params[i];
        struct XrType *ptype = p->type ? p->type : l.type_any;

        XiValue *param_val = xi_param(l.func, entry, (uint16_t) i, ptype);
        l.func->params[i] = param_val;

        /* Register parameter in Braun SSA using analyzer-assigned symbol_id */
        int var_id = xi_lower_var_create(&l, p->symbol_id, p->name, ptype);
        xi_lower_braun_write(&l, var_id, entry, param_val);
    }

    /* For named functions, register a self-reference so the body can
     * resolve recursive calls.  lower_call detects l.self_value and
     * emits a self-call (OP_CALLSELF) instead of a regular call. */
    if (fdecl->name) {
        struct XrType *fn_type = ret_type;  /* approximate; exact type unused */
        XiValue *self = xi_const_null(l.func, entry, l.type_null);
        l.self_value = self;
        int self_var = xi_lower_var_create(&l, fdecl->symbol_id,
                                              fdecl->name, fn_type);
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

    /* Lower function body */
    if (fdecl->body) {
        xi_lower_stmt(&l, fdecl->body);
    }

    /* If last block not terminated, add implicit void return */
    if (l.cur_block) {
        xi_block_set_return(l.cur_block, NULL);
    }

    XiFunc *result = l.had_error ? NULL : l.func;
    if (result) result->stage = XI_STAGE_RAW;
    xi_lower_cleanup(&l);
    return result;
}

/* ========== Public API ========== */

XiFunc *xi_lower_func(AstNode *func_node, struct XaAnalyzer *analyzer,
                       struct XrayIsolate *isolate) {
    XR_CHECK(func_node != NULL, "xi_lower_func: func_node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_func: analyzer is NULL");
    XR_CHECK(func_node->type == AST_FUNCTION_DECL ||
             func_node->type == AST_FUNCTION_EXPR,
             "xi_lower_func: not a function node");
    XiFunc *f = xi_lower_func_impl(func_node, analyzer, isolate, NULL);
    if (f) {
        finalize_capture_metadata(f);
        xi_func_compute_effects(f);
    }
    return f;
}

/*
 * Pre-scan top-level statements to assign shared variable indices.
 * Every named declaration (function, variable, const) at program level
 * gets a shared slot so inner functions can reference them via
 * GETSHARED — including forward references to not-yet-lowered functions.
 */
static void prescan_shared_vars(XiLower *l, AstNode **stmts, int count) {
    XR_DCHECK(l->is_program, "prescan_shared_vars: not a program context");
    uint16_t next_shared = 0;

    /* Temporary array to track which names are exported */
    const char *export_flags[512];
    memset(export_flags, 0, sizeof(export_flags));

    for (int i = 0; i < count; i++) {
        AstNode *s = stmts[i];
        if (!s) continue;
        const char *name = NULL;
        struct XrType *type = l->type_any;
        bool is_exported = false;

        /* Unwrap AST_EXPORT_STMT to reach the inner declaration */
        if (s->type == AST_EXPORT_STMT && s->as.export_stmt.declaration) {
            s = s->as.export_stmt.declaration;
            is_exported = true;
        }

        /* Export-list form: export a, b, c — mark already-assigned shared
         * slots as exported (the declarations were processed earlier). */
        if (s->type == AST_EXPORT_STMT && s->as.export_stmt.export_names) {
            ExportStmtNode *exp = &s->as.export_stmt;
            for (int ei = 0; ei < exp->export_count; ei++) {
                const char *ename = exp->export_names[ei];
                if (!ename) continue;
                int vid = xi_lower_var_find(l, 0, ename);
                if (vid >= 0 && l->shared_map[vid] >= 0) {
                    int slot = l->shared_map[vid];
                    if (slot < 512) export_flags[slot] = ename;
                }
            }
            continue;
        }

        uint32_t sid = 0;
        switch (s->type) {
            case AST_FUNCTION_DECL:
                name = s->as.function_decl.name;
                sid = s->as.function_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                break;
            case AST_CLASS_DECL:
                name = s->as.class_decl.name;
                sid = s->as.class_decl.symbol_id;
                break;
            case AST_STRUCT_DECL:
                name = s->as.struct_decl.name;
                sid = s->as.struct_decl.symbol_id;
                break;
            case AST_VAR_DECL:
            case AST_CONST_DECL:
                name = s->as.var_decl.name;
                sid = s->as.var_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                break;
            case AST_ENUM_DECL:
                name = s->as.enum_decl.name;
                sid = s->as.enum_decl.symbol_id;
                break;
            case AST_IMPORT_STMT:
                if (s->as.import_stmt.member_count == 0) {
                    name = s->as.import_stmt.alias
                         ? s->as.import_stmt.alias
                         : s->as.import_stmt.module_name;
                    sid = s->as.import_stmt.symbol_id;
                } else {
                    /* Selective import: each member gets a shared slot */
                    for (int mi = 0; mi < s->as.import_stmt.member_count; mi++) {
                        ImportMember *m = &s->as.import_stmt.members[mi];
                        const char *mname = m->alias ? m->alias : m->name;
                        if (!mname) continue;
                        int vid = xi_lower_var_create(l, m->symbol_id, mname, type);
                        XR_DCHECK(vid >= 0 && vid < XI_LOWER_MAX_VARS,
                                  "prescan_shared_vars: var_id overflow (import member)");
                        l->shared_map[vid] = (int16_t)next_shared;
                        next_shared++;
                    }
                    continue;
                }
                break;
            default:
                break;
        }
        if (!name) continue;

        /* Create the Braun SSA variable entry (no definition yet) */
        int var_id = xi_lower_var_create(l, sid, name, type);
        XR_DCHECK(var_id >= 0 && var_id < XI_LOWER_MAX_VARS,
                  "prescan_shared_vars: var_id overflow");
        l->shared_map[var_id] = (int16_t)next_shared;
        if (is_exported && next_shared < 512)
            export_flags[next_shared] = name;
        next_shared++;
    }
    l->func->nshared = next_shared;

    /* Populate export_names on XiFunc for cross-module resolution.
     * Copy name strings into the arena so they survive AST destruction. */
    if (next_shared > 0) {
        const char **names = (const char **)xi_func_arena_alloc(
            l->func, (uint32_t)(next_shared * sizeof(const char *)));
        if (names) {
            for (uint16_t si = 0; si < next_shared; si++) {
                const char *src = (si < 512) ? export_flags[si] : NULL;
                if (src) {
                    uint32_t slen = (uint32_t)strlen(src);
                    char *copy = (char *)xi_func_arena_alloc(l->func, slen + 1);
                    if (copy) { memcpy(copy, src, slen + 1); }
                    names[si] = copy;
                } else {
                    names[si] = NULL;
                }
            }
            l->func->export_names = names;
        }
    }
}

/*
 * Recursively decorate capture metadata on the function tree.
 * Sets capture_kind and is_mutable based on the already-computed needs_cell
 * flag from the lowering-time closure analysis.  This finalizes the metadata
 * so downstream passes (emit, JIT, AOT) can read XiCapture.capture_kind
 * instead of interpreting needs_cell + source heuristically.
 */
static void finalize_capture_metadata(XiFunc *f) {
    XR_DCHECK(f != NULL, "finalize_capture_metadata: NULL func");

    for (uint16_t i = 0; i < f->ncaptures; i++) {
        XiCapture *cap = &f->captures[i];
        if (cap->needs_cell) {
            cap->capture_kind = (uint8_t)XI_CAPTURE_BY_MUT_CELL;
            cap->is_mutable = true;
        } else {
            cap->capture_kind = (uint8_t)XI_CAPTURE_BY_COPY;
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
 * Constructs the exports table from export_names + shared_slot_funcs/classes
 * without scanning IR instructions.  Also collects class data into
 * module->classes for AOT codegen.
 */
static void build_module_metadata(XiLower *l) {
    XiFunc *f = l->func;
    XR_DCHECK(f != NULL, "build_module_metadata: NULL func");

    uint16_t nshared = f->nshared;
    if (nshared == 0 && f->nchildren == 0) return;

    /* Allocate module (caller must free via xi_module_free) */
    XiModule *mod = xi_module_new(NULL, NULL, f);
    if (!mod) return;

    /* Build exports from export_names + tracked function/class pointers */
    if (f->export_names && nshared > 0) {
        uint16_t nexports = 0;
        for (uint16_t s = 0; s < nshared; s++) {
            if (f->export_names[s]) nexports++;
        }
        if (nexports > 0) {
            XiModuleExport *exps = (XiModuleExport *)xr_calloc(
                nexports, sizeof(XiModuleExport));
            if (exps) {
                uint16_t ei = 0;
                for (uint16_t s = 0; s < nshared && ei < nexports; s++) {
                    if (!f->export_names[s]) continue;
                    exps[ei].name = f->export_names[s];
                    exps[ei].shared_slot = s;
                    exps[ei].function = l->shared_slot_funcs[s];
                    exps[ei].class_data = l->shared_slot_classes[s];
                    /* Type info from the var entry that maps to this slot */
                    for (int vi = 0; vi < l->var_count; vi++) {
                        if (l->shared_map[vi] == (int16_t)s) {
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
        if (l->shared_slot_classes[s]) class_count++;
    }
    if (class_count > 0) {
        XiClassData **cls = (XiClassData **)xr_calloc(
            class_count, sizeof(XiClassData *));
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

    /* xi_module_new already copies init->children into mod->functions */
    f->module = mod;
}

XiFunc *xi_lower_program(AstNode *program_node, struct XaAnalyzer *analyzer,
                          struct XrayIsolate *isolate) {
    XR_CHECK(program_node != NULL, "xi_lower_program: node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_program: analyzer is NULL");

    XiLower l;
    xi_lower_init(&l, analyzer, isolate);
    l.is_program = true;

    l.func = xi_func_new("<main>", l.type_void);
    if (!l.func) { xi_lower_cleanup(&l); return NULL; }
    l.func->analyzer = analyzer;
    l.func->nparams = 0;
    l.func->params = NULL;

    XiBlock *entry = xi_block_new(l.func);
    entry->sealed = true;
    l.cur_block = entry;

    /* Pre-scan: assign shared indices to all top-level declarations */
    AstNode **stmts;
    int count;
    if (program_node->type == AST_BLOCK) {
        stmts = program_node->as.block.statements;
        count = program_node->as.block.count;
    } else {
        stmts = program_node->as.program.statements;
        count = program_node->as.program.count;
    }
    prescan_shared_vars(&l, stmts, count);

    /* Lower all top-level statements */
    for (int i = 0; i < count; i++) {
        if (!l.cur_block) break;
        xi_lower_stmt(&l, stmts[i]);
    }

    if (l.cur_block) {
        xi_block_set_return(l.cur_block, NULL);
    }

    /* Build module metadata from lowerer tracking data (no IR scan needed) */
    if (!l.had_error) {
        build_module_metadata(&l);
        finalize_capture_metadata(l.func);
        xi_func_compute_effects(l.func);
    }

    XiFunc *result = l.had_error ? NULL : l.func;
    if (result) result->stage = XI_STAGE_RAW;
    xi_lower_cleanup(&l);
    return result;
}
