/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi.c - Typed SSA IR core operations
 *
 * Provides create/add/free functions for XiFunc, XiBlock, XiValue, XiPhi.
 * All IR nodes are bump-allocated from a per-function arena so destruction
 * is O(1) (free the arena chunk). Individual node frees are no-ops.
 */

#include "xi.h"
#include "../plan/semantic/xr_program_semantic_closure.h"
#include "../plan/target/xr_i64_overflow_decision.h"
#include "../plan/target/xr_target_profile.h"
#include "xi_evidence.h"
#include "xi_loop.h"
#include "xi_module.h"
#include "xi_range.h"
#include "xi_effect.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include "../runtime/value/xtype.h" /* XR_REP_TAGGED */
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../module/xmodule_identity.h"

#include <string.h>
#include <stdio.h>

/* ========== Arena Allocator ========== */

/* A linked list of fixed-size chunks. Once an allocation is handed
 * out, the underlying memory is never moved, so XiValue/XiBlock/XiPhi
 * pointers remain valid for the lifetime of the XiFunc. */
typedef struct XiArenaChunk {
    struct XiArenaChunk *next;
    uint32_t used;
    uint32_t cap;
    /* data follows */
} XiArenaChunk;

static XiArenaChunk *arena_chunk_new(uint32_t cap) {
    XiArenaChunk *c = (XiArenaChunk *) xr_malloc(sizeof(XiArenaChunk) + cap);
    if (!c)
        return NULL;
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    return c;
}

static inline uint8_t *arena_chunk_data(XiArenaChunk *c) {
    return (uint8_t *) (c + 1);
}

static void *arena_alloc(XiFunc *f, uint32_t size) {
    XR_DCHECK(f != NULL, "arena_alloc: f is NULL");
    /* Align to 8 bytes */
    size = (size + 7) & ~7u;

    XiArenaChunk *cur = f->arena_cur;
    if (!cur || cur->used + size > cur->cap) {
        /* Need a new chunk. Default chunk size is XI_ARENA_INITIAL_SIZE,
         * but oversized requests get a dedicated chunk sized to fit. */
        uint32_t new_cap = XI_ARENA_INITIAL_SIZE;
        if (size > new_cap)
            new_cap = size;
        XiArenaChunk *nc = arena_chunk_new(new_cap);
        if (!nc) {
            fprintf(stderr, "xi: arena chunk alloc failed (requested %u bytes)\n", new_cap);
            return NULL;
        }
        if (cur) {
            cur->next = nc;
        } else {
            f->arena_head = nc;
        }
        f->arena_cur = nc;
        cur = nc;
    }

    void *ptr = arena_chunk_data(cur) + cur->used;
    memset(ptr, 0, size);
    cur->used += size;
    return ptr;
}

void *xi_func_arena_alloc(XiFunc *f, uint32_t size) {
    return arena_alloc(f, size);
}

bool xi_value_clone_call_plan(XiFunc *f, XiValue *dst, const XiValue *src) {
    if (!f || !dst || !src)
        return false;
    dst->call_plan = NULL;
    if (!src->call_plan)
        return true;
    if ((dst->op != XI_CALL && dst->op != XI_CALL_METHOD && dst->op != XI_CALL_METHOD_DIRECT &&
         dst->op != XI_TAIL_CALL) ||
        dst->nargs < 1 || src->call_plan->nargs != (uint16_t) (dst->nargs - 1) ||
        (src->call_plan->nargs > 0 && !src->call_plan->args))
        return false;

    XiCallPlan *plan = (XiCallPlan *) xi_func_arena_alloc(f, sizeof(*plan));
    if (!plan)
        return false;
    *plan = *src->call_plan;
    plan->args = NULL;
    if (plan->has_receiver)
        plan->receiver.place = dst->args[0];
    if (plan->nargs > 0) {
        plan->args = (XiCallArgPlan *) xi_func_arena_alloc(
            f, (uint32_t) ((size_t) plan->nargs * sizeof(*plan->args)));
        if (!plan->args)
            return false;
        memcpy(plan->args, src->call_plan->args, (size_t) plan->nargs * sizeof(*plan->args));
        for (uint16_t i = 0; i < plan->nargs; i++) {
            plan->args[i].place = src->call_plan->args[i].place ? dst->args[i + 1] : NULL;
        }
    }
    dst->call_plan = plan;
    return true;
}

/* A plan outlives the AST that produced it, so the source path it points at
 * must be owned by the same arena as the plan. Both plan families need this,
 * so the ownership rule is written once. */
static const char *xi_func_arena_own_source_file(XiFunc *f, const char *file) {
    if (!f || !file)
        return NULL;
    size_t length = strlen(file);
    if (length >= UINT32_MAX)
        return NULL;
    char *owned = (char *) xi_func_arena_alloc(f, (uint32_t) length + 1u);
    if (!owned)
        return NULL;
    memcpy(owned, file, length + 1u);
    return owned;
}

bool xi_value_set_assertion_plan(XiFunc *f, XiValue *value, const XrAssertionPlan *plan) {
    if (!f || !value || value->op != XI_ASSERTION || !plan || !xr_assertion_plan_validate(plan))
        return false;
    XrAssertionPlan *owned = (XrAssertionPlan *) xi_func_arena_alloc(f, (uint32_t) sizeof(*owned));
    if (!owned)
        return false;
    *owned = *plan;
    owned->source.file = xi_func_arena_own_source_file(f, plan->source.file);
    if (!owned->source.file || !xr_assertion_plan_validate(owned))
        return false;
    value->aux = owned;
    value->aux_kind = XI_AUX_KIND_ASSERTION_PLAN;
    return true;
}

bool xi_value_set_print_plan(XiFunc *f, XiValue *value, const XrPrintPlan *plan) {
    if (!f || !value || value->op != XI_PRINT || !plan || !xr_print_plan_validate(plan))
        return false;
    XrPrintPlan *owned = (XrPrintPlan *) xi_func_arena_alloc(f, (uint32_t) sizeof(*owned));
    if (!owned)
        return false;
    *owned = *plan;
    owned->source.file = xi_func_arena_own_source_file(f, plan->source.file);
    if (!owned->source.file || !xr_print_plan_validate(owned))
        return false;
    value->aux = owned;
    value->aux_kind = XI_AUX_KIND_PRINT_PLAN;
    return true;
}

bool xi_value_clone_assertion_plan(XiFunc *f, XiValue *dst, const XiValue *src) {
    if (!f || !dst || !src)
        return false;
    if (src->op != XI_ASSERTION)
        return src->aux_kind != XI_AUX_KIND_ASSERTION_PLAN;
    const XrAssertionPlan *plan = xi_assertion_plan(src);
    return plan && xi_value_set_assertion_plan(f, dst, plan);
}

bool xi_value_clone_print_plan(XiFunc *f, XiValue *dst, const XiValue *src) {
    if (!f || !dst || !src)
        return false;
    if (src->op != XI_PRINT)
        return src->aux_kind != XI_AUX_KIND_PRINT_PLAN;
    const XrPrintPlan *plan = xi_print_plan(src);
    return plan && xi_value_set_print_plan(f, dst, plan);
}

bool xi_value_clone_metadata(XiFunc *f, XiValue *dst, const XiValue *src) {
    if (!f || !dst || !src || dst->op != src->op)
        return false;
    xi_value_copy_metadata(dst, src);
    if (src->aux_kind == XI_AUX_KIND_ASSERTION_PLAN)
        return xi_value_clone_assertion_plan(f, dst, src);
    if (src->aux_kind == XI_AUX_KIND_PRINT_PLAN)
        return xi_value_clone_print_plan(f, dst, src);
    return true;
}

XR_FUNC bool xi_func_set_param_passing_mode(XiFunc *f, uint16_t index, XrParamMode mode) {
    if (!f || index >= f->nparams || !f->params || !f->params[index])
        return false;
    if (!xr_param_mode_is_valid(mode))
        return false;
    f->params[index]->param_mode = (uint8_t) mode;
    return true;
}

XR_FUNC XrParamMode xi_func_param_passing_mode(const XiFunc *f, uint16_t index) {
    if (!f || index >= f->nparams || !f->params || !f->params[index])
        return XR_PARAM_READ;
    return (XrParamMode) f->params[index]->param_mode;
}

XR_FUNC bool xi_block_ensure_value_capacity(XiBlock *blk, uint32_t min_cap) {
    XR_DCHECK(blk != NULL, "xi_block_ensure_value_capacity: blk is NULL");
    XR_DCHECK(blk->func != NULL, "xi_block_ensure_value_capacity: blk->func is NULL");

    if (min_cap <= blk->values_cap)
        return true;

    uint32_t new_cap = blk->values_cap ? blk->values_cap : 16;
    while (new_cap < min_cap) {
        if (new_cap > UINT32_MAX / 2) {
            new_cap = min_cap;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap > UINT32_MAX / (uint32_t) sizeof(XiValue *))
        return false;

    XiValue **new_values =
        (XiValue **) xi_func_arena_alloc(blk->func, new_cap * (uint32_t) sizeof(XiValue *));
    if (!new_values)
        return false;
    if (blk->values && blk->nvalues > 0)
        memcpy(new_values, blk->values, blk->nvalues * sizeof(XiValue *));
    blk->values = new_values;
    blk->values_cap = new_cap;
    return true;
}

XR_FUNC void xi_func_compute_effects(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_func_compute_effects: NULL func");
    uint8_t summary = 0;

    /* OR all value flags in this function */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v)
                summary |= v->flags;
        }
    }

    /* Recurse into children; propagate MAY_SUSPEND upward since a
     * parent that calls a may-suspend child is itself may-suspend. */
    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (!f->children[ci])
            continue;
        xi_func_compute_effects(f->children[ci]);
        if (f->children[ci]->effect_summary & XI_FLAG_MAY_SUSPEND)
            summary |= XI_FLAG_MAY_SUSPEND;
    }

    f->effect_summary = summary;
}

static void arena_free_all(XiFunc *f) {
    XiArenaChunk *c = f->arena_head;
    while (c) {
        XiArenaChunk *next = c->next;
        xr_free(c);
        c = next;
    }
    f->arena_head = NULL;
    f->arena_cur = NULL;
}

/* ========== Function Lifecycle ========== */

XiFunc *xi_func_new(const char *name, struct XrType *return_type) {
    XiFunc *f = (XiFunc *) xr_calloc(1, sizeof(XiFunc));
    if (!f)
        return NULL;

    f->return_type = return_type;
    f->view_return_param = -1;
    f->arc_return_ownership.param_index = -1;
    f->semantic_plan_function_index = XR_SEMANTIC_INDEX_NONE;
    f->psc_function_index = XI_PSC_ROW_NONE;
    f->psc_return_type_index = XI_PSC_ROW_NONE;
    f->stage = XI_STAGE_RAW;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);
    /* Start cfg_version at 1 so the calloc-zeroed rpo/dom versions
     * deterministically trigger the first xi_ensure_*() recompute. */
    f->cfg_version = 1;
    xi_evidence_init_func(f);

    /* Initialize arena: allocate first chunk eagerly so the common
     * fast path in arena_alloc avoids a NULL check on every call. */
    f->arena_head = arena_chunk_new(XI_ARENA_INITIAL_SIZE);
    if (!f->arena_head) {
        xr_free(f);
        return NULL;
    }
    f->arena_cur = f->arena_head;

    /* Copy name into arena so it survives AST destruction */
    if (name) {
        uint32_t len = (uint32_t) strlen(name);
        char *copy = (char *) xi_func_arena_alloc(f, len + 1);
        if (copy) {
            memcpy(copy, name, len + 1);
            f->name = copy;
        }
    }

    /* Initial block capacity */
    f->blocks_cap = 16;
    f->blocks = (XiBlock **) xr_calloc(f->blocks_cap, sizeof(XiBlock *));
    if (!f->blocks) {
        arena_free_all(f);
        xr_free(f);
        return NULL;
    }

    return f;
}

void xi_func_free(XiFunc *f) {
    if (!f)
        return;

    xr_semantic_plan_free(f->semantic_plan);
    f->semantic_plan = NULL;
    f->semantic_plan_function_index = XR_SEMANTIC_INDEX_NONE;

    /* Free nested children recursively */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        xi_func_free(f->children[i]);
    }
    xr_free(f->children);

    /* XiFunc is the sole owner; XiModule.closure_metas is a lookup-only
     * inventory of these pointers. */
    xr_free(f->closure_meta);
    f->closure_meta = NULL;

    /* Free attached module metadata (only set on program-level init funcs) */
    if (f->module) {
        /* Null out init to avoid xi_module_free trying to recurse into us */
        f->module->init = NULL;
        xi_module_free(f->module);
        f->module = NULL;
    }

    /* Release the lazily-cached loop forest, if any. */
    if (f->loop_cache) {
        xi_loopinfo_free(f->loop_cache);
        f->loop_cache = NULL;
    }

    xi_range_dispose(f);

    xi_evidence_dispose_func(f);

    /* Arena owns all XiValues, XiPhis, XiBlocks, and arg arrays.
     * blocks[] (the index array) is heap-allocated separately because
     * it must be resizable across realloc without invalidating IR
     * pointers. */
    arena_free_all(f);
    xr_free(f->blocks);
    xr_free(f->params);
    xr_free(f);
}

/* ========== Block Management ========== */

XiBlock *xi_block_new(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_block_new: f is NULL");

    XiBlock *blk = (XiBlock *) arena_alloc(f, sizeof(XiBlock));
    if (!blk)
        return NULL;

    blk->id = f->next_block_id++;
    blk->kind = XI_BLOCK_PLAIN;
    blk->func = f;

    /* Initial values capacity */
    blk->values_cap = 16;
    blk->values = (XiValue **) arena_alloc(f, blk->values_cap * sizeof(XiValue *));

    /* Initial preds capacity */
    blk->preds_cap = 4;
    blk->preds = (XiBlock **) arena_alloc(f, blk->preds_cap * sizeof(XiBlock *));

    /* Register in function */
    if (f->nblocks >= f->blocks_cap) {
        uint32_t new_cap = f->blocks_cap * 2;
        XiBlock **tmp = (XiBlock **) xr_realloc(f->blocks, new_cap * sizeof(XiBlock *));
        if (!tmp)
            return NULL;
        f->blocks = tmp;
        f->blocks_cap = new_cap;
    }
    f->blocks[f->nblocks++] = blk;

    /* First block is the entry */
    if (f->nblocks == 1)
        f->entry = blk;

    return blk;
}

void xi_block_add_pred(XiBlock *blk, XiBlock *pred) {
    XR_DCHECK(blk != NULL, "xi_block_add_pred: blk is NULL");
    XR_DCHECK(pred != NULL, "xi_block_add_pred: pred is NULL");

    if (blk->npreds >= blk->preds_cap) {
        /* Arena-allocated arrays cannot be resized in place.
         * Allocate a new larger array and copy. Old memory is wasted
         * but reclaimed when the arena is freed. */
        uint16_t new_cap = blk->preds_cap * 2;
        XiBlock **new_preds = (XiBlock **) arena_alloc(blk->func, new_cap * sizeof(XiBlock *));
        if (!new_preds)
            return;
        memcpy(new_preds, blk->preds, blk->npreds * sizeof(XiBlock *));
        blk->preds = new_preds;
        blk->preds_cap = new_cap;
    }
    blk->preds[blk->npreds++] = pred;
}

/* ========== Value Construction ========== */

/* Initialize the scalar fields every XiValue needs before any pass (notably
 * the register allocator) reads them. The arena is NOT zero-initialized
 * (xr_malloc), so this MUST set every field whose correct default is non-zero:
 * var_id (XI_NO_VAR_ID "no source variable", NOT 0 which is variable #0), rep
 * (XR_REP_TAGGED) and uses (-1). Shared by value_alloc() and xi_phi_new() so
 * the two construction paths can never drift — a missing var_id init in the
 * phi path silently coalesced temporary phis (&& / || / ternary results) onto
 * variable #0's register in reg_of(), clobbering that live local. The caller
 * assigns args[] (sized differently for phis vs. regular values). */
static inline void xi_value_init_fields(XiValue *v, uint32_t id, uint16_t op, struct XrType *type,
                                        uint16_t nargs, XiBlock *blk, XiSourceSpan source_span) {
    v->id = id;
    v->op = op;
    v->flags = xi_op_default_effects(op);
    v->rep = XR_REP_TAGGED;   /* default until select_rep assigns concrete rep */
    v->var_id = XI_NO_VAR_ID; /* no source variable */
    v->transfer_mode = 0;
    v->aux_kind = XI_AUX_KIND_NONE;
    v->escape = 0;
    v->mem_group = 0;
    v->lowering_flags = 0;
    v->param_mode = XR_PARAM_READ;
    v->type = type;
    v->aux_int = 0;
    v->aux = NULL;
    memset(&v->conversion, 0, sizeof(v->conversion));
    v->call_return_ownership.param_index = -1;
    v->result_alias_operand = -1;
    v->args = NULL;
    v->nargs = nargs;
    v->uses = -1; /* not yet computed */
    v->line = 0;
    v->source_kind = 0;
    v->source_span = source_span;
    v->psc_call_index = XI_PSC_ROW_NONE;
    v->psc_type_index = XI_PSC_ROW_NONE;
    v->xg_callsite_id = 0;
    v->xa_intrinsic_id = 0;
    v->array_intrinsic_kind = XI_ARRAY_INTRINSIC_NONE;
    v->array_member_kind = XI_ARRAY_MEMBER_NONE;
    v->array_hof_kind = XI_ARRAY_HOF_NONE;
    v->array_element_storage = XR_ELEM_ANY;
    v->array_result_element_storage = XR_ELEM_ANY;
    v->xg_method_id = 0;
    v->xg_interface_dispatch_slot = UINT32_MAX;
    v->xg_json_codec_id = 0;
    v->xg_object_access_id = 0;
    v->xg_object_merge_id = 0;
    v->xg_key_access_id = 0;
    v->xg_map_shape_id = 0;
    v->xg_class_field_id = 0;
    v->xg_sequence_access_id = 0;
    v->xg_capacity_op_id = 0;
    v->xg_bulk_op_id = 0;
    v->xg_encoding_op_id = 0;
    v->block = blk;
}

static XiValue *value_alloc(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type,
                            uint16_t nargs) {
    XR_DCHECK(f != NULL, "value_alloc: f is NULL");
    XR_DCHECK(blk != NULL, "value_alloc: blk is NULL");
    XR_DCHECK(type != NULL, "value_alloc: type is NULL");

    XiValue *v = (XiValue *) arena_alloc(f, sizeof(XiValue));
    if (!v)
        return NULL;

    xi_value_init_fields(v, f->next_value_id++, op, type, nargs, blk, f->lowering_source_span);

    if (nargs > 0) {
        v->args = (XiValue **) arena_alloc(f, nargs * sizeof(XiValue *));
    }

    return v;
}

static void block_append_value(XiBlock *blk, XiValue *v) {
    XR_DCHECK(blk != NULL, "block_append_value: blk is NULL");
    XR_DCHECK(v != NULL, "block_append_value: v is NULL");

    if (!xi_block_ensure_value_capacity(blk, blk->nvalues + 1))
        return;
    blk->values[blk->nvalues++] = v;
}

XiValue *xi_value_new(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type, uint16_t nargs) {
    XiValue *v = value_alloc(f, blk, op, type, nargs);
    if (!v)
        return NULL;
    block_append_value(blk, v);
    return v;
}

XiValue *xi_value_new_unlinked(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type,
                               uint16_t nargs) {
    return value_alloc(f, blk, op, type, nargs);
}

/* Create a value and splice it into blk immediately AFTER `anchor`.
 * If `anchor` is NULL or not found, the value is appended at the end.
 * Used by ownership/ARC insertion to place dup/drop at a precise point
 * (e.g. a drop right after a value's last use). O(n) splice. */
XiValue *xi_value_insert_after(XiFunc *f, XiBlock *blk, XiValue *anchor, uint16_t op,
                               struct XrType *type, uint16_t nargs) {
    XiValue *v = value_alloc(f, blk, op, type, nargs);
    if (!v)
        return NULL;

    if (!xi_block_ensure_value_capacity(blk, blk->nvalues + 1))
        return NULL;

    /* Find anchor index; default to end (append) if absent. */
    uint32_t pos = blk->nvalues; /* insert position = after anchor */
    if (anchor) {
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] == anchor) {
                pos = i + 1;
                break;
            }
        }
    }

    /* Shift tail right by one, then place v at pos. */
    for (uint32_t i = blk->nvalues; i > pos; i--)
        blk->values[i] = blk->values[i - 1];
    blk->values[pos] = v;
    blk->nvalues++;
    return v;
}

XiValue *xi_value_insert_before(XiFunc *f, XiBlock *blk, XiValue *anchor, uint16_t op,
                                struct XrType *type, uint16_t nargs) {
    XiValue *v = value_alloc(f, blk, op, type, nargs);
    if (!v)
        return NULL;
    if (!xi_block_ensure_value_capacity(blk, blk->nvalues + 1))
        return NULL;

    uint32_t pos = blk->nvalues;
    if (anchor) {
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] == anchor) {
                pos = i;
                break;
            }
        }
    }
    for (uint32_t i = blk->nvalues; i > pos; i--)
        blk->values[i] = blk->values[i - 1];
    blk->values[pos] = v;
    blk->nvalues++;
    return v;
}

/* ========== Constant Constructors ========== */

XiValue *xi_const_int(XiFunc *f, XiBlock *blk, int64_t val, struct XrType *int_type) {
    XR_DCHECK(int_type != NULL, "xi_const_int: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, int_type, 0);
    if (v)
        v->aux_int = val;
    return v;
}

XiValue *xi_const_float(XiFunc *f, XiBlock *blk, double val, struct XrType *float_type) {
    XR_DCHECK(float_type != NULL, "xi_const_float: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, float_type, 0);
    if (v) {
        /* Store double bits in aux_int to avoid union aliasing issues */
        memcpy(&v->aux_int, &val, sizeof(double));
    }
    return v;
}

XiValue *xi_const_bool(XiFunc *f, XiBlock *blk, bool val, struct XrType *bool_type) {
    XR_DCHECK(bool_type != NULL, "xi_const_bool: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, bool_type, 0);
    if (v)
        v->aux_int = val ? 1 : 0;
    return v;
}

XiValue *xi_const_rune(XiFunc *f, XiBlock *blk, uint32_t val, struct XrType *rune_type) {
    XR_DCHECK(rune_type != NULL, "xi_const_rune: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, rune_type, 0);
    if (v)
        v->aux_int = (int64_t) val;
    return v;
}

XiValue *xi_const_null(XiFunc *f, XiBlock *blk, struct XrType *null_type) {
    XR_DCHECK(null_type != NULL, "xi_const_null: type is NULL");
    return xi_value_new(f, blk, XI_CONST, null_type, 0);
}

XiValue *xi_const_str(XiFunc *f, XiBlock *blk, const char *str, struct XrType *str_type) {
    XR_DCHECK(str_type != NULL, "xi_const_str: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, str_type, 0);
    if (v && str) {
        /* Copy into arena so the string survives AST destruction */
        uint32_t len = (uint32_t) strlen(str);
        char *copy = (char *) xi_func_arena_alloc(f, len + 1);
        if (copy)
            memcpy(copy, str, len + 1);
        v->aux = (void *) copy;
    }
    return v;
}

XiValue *xi_const_bigint(XiFunc *f, XiBlock *blk, const char *digits, struct XrType *bigint_type) {
    XR_DCHECK(bigint_type != NULL, "xi_const_bigint: type is NULL");
    XR_DCHECK(digits != NULL, "xi_const_bigint: digits is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, bigint_type, 0);
    if (v && digits) {
        /* Copy decimal digit string into arena (same approach as xi_const_str) */
        uint32_t len = (uint32_t) strlen(digits);
        char *copy = (char *) xi_func_arena_alloc(f, len + 1);
        if (copy)
            memcpy(copy, digits, len + 1);
        v->aux = (void *) copy;
        /* Say what the payload is, so later layers read the digits by this
         * mark rather than by recognising the type's name. This is the only
         * place a BigInt constant is built. */
        v->aux_kind = XI_AUX_KIND_BIGINT_DIGITS;
    }
    return v;
}

/* ========== Convenience Constructors ========== */

XiValue *xi_binary(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type, XiValue *lhs,
                   XiValue *rhs) {
    XR_DCHECK(lhs != NULL, "xi_binary: lhs is NULL");
    XR_DCHECK(rhs != NULL, "xi_binary: rhs is NULL");
    XiValue *v = xi_value_new(f, blk, op, type, 2);
    if (v) {
        v->args[0] = lhs;
        v->args[1] = rhs;
        /* Division/modulo may throw on zero divisor — must not be DCE'd */
        if (op == XI_DIV || op == XI_MOD)
            v->flags |= XI_FLAG_MAY_THROW;
        /* XI_EQ / XI_NE are polymorphic: pure for primitives but
         * deep-equality (reads heap content) for arrays/maps/objects/
         * strings.  Without READS_MEM the shuffler / DCE / LICM may
         * reorder the comparison across a mutating store, silently
         * changing semantics (regression: 0680_deep_eq_cyclic). */
        if (op == XI_EQ || op == XI_NE) {
            const struct XrType *types[2] = {lhs->type, rhs->type};
            for (int i = 0; i < 2; i++) {
                const struct XrType *t = types[i];
                if (!t)
                    continue;
                XrTypeKind k = t->kind;
                if (k != XR_KIND_INT && k != XR_KIND_FLOAT && k != XR_KIND_BOOL &&
                    k != XR_KIND_RUNE && k != XR_KIND_NULL && k != XR_KIND_UNIT &&
                    k != XR_KIND_ENUM) {
                    v->flags |= XI_FLAG_READS_MEM;
                    break;
                }
            }
        }
    }
    return v;
}

XiValue *xi_unary(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type, XiValue *arg) {
    XR_DCHECK(arg != NULL, "xi_unary: arg is NULL");
    XiValue *v = xi_value_new(f, blk, op, type, 1);
    if (v)
        v->args[0] = arg;
    return v;
}

XiValue *xi_param(XiFunc *f, XiBlock *blk, uint16_t index, struct XrType *type) {
    XiValue *v = xi_value_new(f, blk, XI_PARAM, type, 0);
    if (v)
        v->aux_int = index;
    return v;
}

/* ========== Phi Nodes ========== */

XiPhi *xi_phi_new(XiFunc *f, XiBlock *blk, struct XrType *type, uint16_t npreds) {
    XR_DCHECK(f != NULL, "xi_phi_new: f is NULL");
    XR_DCHECK(blk != NULL, "xi_phi_new: blk is NULL");
    XR_DCHECK(type != NULL, "xi_phi_new: type is NULL");

    XiPhi *phi = (XiPhi *) arena_alloc(f, sizeof(XiPhi));
    if (!phi)
        return NULL;

    /* Shared field init (var_id=XI_NO_VAR_ID etc.); a real variable phi's var_id is
     * overwritten by the Braun SSA construction after this call. */
    xi_value_init_fields(&phi->value, f->next_value_id++, XI_PHI, type, npreds, blk,
                         f->lowering_source_span);

    if (npreds > 0) {
        phi->value.args = (XiValue **) arena_alloc(f, npreds * sizeof(XiValue *));
    }

    /* Prepend to block phi list */
    phi->next = blk->phis;
    blk->phis = phi;

    return phi;
}

/* ========== Block Termination ========== */

void xi_block_set_return(XiBlock *blk, XiValue *val) {
    XR_DCHECK(blk != NULL, "xi_block_set_return: blk is NULL");
    blk->kind = XI_BLOCK_RETURN;
    blk->control = val; /* NULL for void return */
    blk->succs[0] = NULL;
    blk->succs[1] = NULL;
}

void xi_block_set_jump(XiBlock *blk, XiBlock *target) {
    XR_DCHECK(blk != NULL, "xi_block_set_jump: blk is NULL");
    XR_DCHECK(target != NULL, "xi_block_set_jump: target is NULL");
    blk->kind = XI_BLOCK_PLAIN;
    blk->control = NULL;
    blk->line = 0;
    blk->succs[0] = target;
    blk->succs[1] = NULL;
    xi_block_add_pred(target, blk);
}

void xi_block_set_if(XiBlock *blk, XiValue *cond, XiBlock *then_blk, XiBlock *else_blk) {
    XR_DCHECK(blk != NULL, "xi_block_set_if: blk is NULL");
    XR_DCHECK(cond != NULL, "xi_block_set_if: cond is NULL");
    XR_DCHECK(then_blk != NULL, "xi_block_set_if: then_blk is NULL");
    XR_DCHECK(else_blk != NULL, "xi_block_set_if: else_blk is NULL");
    blk->kind = XI_BLOCK_IF;
    blk->control = cond;
    blk->line = 0;
    blk->succs[0] = then_blk;
    blk->succs[1] = else_blk;
    xi_block_add_pred(then_blk, blk);
    xi_block_add_pred(else_blk, blk);
}

/* ========== Module ========== */

XR_FUNC XiModule *xi_module_new(const char *path, const char *name, XiFunc *init) {
    XR_DCHECK(init != NULL, "xi_module_new: NULL init func");
    XiModule *mod = (XiModule *) xr_calloc(1, sizeof(XiModule));
    if (!mod)
        return NULL;
    mod->path = path;
    mod->name = name;
    mod->init = init;
    mod->psc_module_index = XI_PSC_ROW_NONE;
    /* Populate functions array from init's children */
    if (init->nchildren > 0) {
        mod->functions = (XiFunc **) xr_calloc(init->nchildren, sizeof(XiFunc *));
        if (mod->functions) {
            for (uint16_t i = 0; i < init->nchildren; i++)
                mod->functions[i] = init->children[i];
            mod->nfuncs = init->nchildren;
        }
    }
    return mod;
}

XR_FUNC bool xi_module_set_identity(XiModule *mod, const char *identity) {
    if (!mod || !xr_module_identity_valid(identity, NULL))
        return false;
    char *owned = xr_strdup(identity);
    if (!owned)
        return false;
    xr_free(mod->identity);
    mod->identity = owned;
    return true;
}

XR_FUNC void xi_module_free(XiModule *mod) {
    if (!mod)
        return;
    xr_free(mod->scalar_call_decision);
    mod->scalar_call_decision = NULL;
    if (mod->i64_overflow_decisions) {
        xr_i64_overflow_decision_dispose(mod->i64_overflow_decisions);
        xr_free(mod->i64_overflow_decisions);
        mod->i64_overflow_decisions = NULL;
    }
    xr_target_profile_free(mod->scalar_target_profile);
    mod->scalar_target_profile = NULL;
    xr_program_semantic_closure_free(mod->program_semantic_closure);
    mod->program_semantic_closure = NULL;
    xr_free(mod->identity);
    xr_free(mod->functions);
    xr_free(mod->exports);
    xr_free(mod->classes);
    xr_free(mod->slot_funcs);
    xr_free(mod->slot_classes);
    xr_free(mod->slot_enums);
    xr_free(mod->slot_imports);
    xr_free(mod->slot_const_literals);
    xr_free(mod->slot_shared_initializers);
    xr_free(mod->global_asm_templates);
    /* Entries are borrowed from XiFunc; only the lookup array is module-owned. */
    xr_free(mod->closure_metas);
    xr_free(mod);
}
