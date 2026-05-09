/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm.c - Xm core data structures and operations
 *
 * KEY CONCEPT:
 *   Arena-based allocation for all IR nodes. Everything in a XmFunc
 *   is allocated from its arena and freed in one shot.
 */

#include "xm.h"
#include "xm_domtree.h"
#include "xm_looptree.h"
#include "xm_alias.h"
#include "xm_defuse.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Arena Allocator ========== */

static XmArenaPage *arena_new_page(size_t min_size) {
    size_t page_size = min_size < XM_ARENA_PAGE_SIZE ? XM_ARENA_PAGE_SIZE : min_size;
    XmArenaPage *page = (XmArenaPage *) xr_malloc(sizeof(XmArenaPage) + page_size);
    if (!page)
        return NULL;
    page->next = NULL;
    page->used = 0;
    page->size = page_size;
    return page;
}

void xm_arena_init(XmArena *arena) {
    XR_DCHECK(arena != NULL, "xm_arena_init: NULL arena");
    arena->current = NULL;
    arena->pages = NULL;
    arena->total_allocated = 0;
}

void xm_arena_destroy(XmArena *arena) {
    XmArenaPage *page = arena->pages;
    while (page) {
        XmArenaPage *next = page->next;
        xr_free(page);
        page = next;
    }
    arena->current = NULL;
    arena->pages = NULL;
    arena->total_allocated = 0;
}

void *xm_arena_alloc(XmArena *arena, size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~(size_t) 7;

    if (arena->current && arena->current->used + size <= arena->current->size) {
        void *ptr = arena->current->data + arena->current->used;
        arena->current->used += size;
        return ptr;
    }

    // Need new page
    XmArenaPage *page = arena_new_page(size + sizeof(XmArenaPage));
    if (!page)
        return NULL;

    page->next = arena->pages;
    arena->pages = page;
    arena->current = page;
    arena->total_allocated += page->size;

    void *ptr = page->data;
    page->used = size;
    return ptr;
}

/* ========== XmFunc ========== */

#define INIT_BLOCKS 16
#define INIT_VREGS 64
#define INIT_CONSTS 32
#define INIT_INS 32
#define INIT_PREDS 4

XmFunc *xm_func_new(const char *name) {
    XmFunc *func = (XmFunc *) xr_calloc(1, sizeof(XmFunc));
    if (!func)
        return NULL;

    func->arena = (XmArena *) xr_malloc(sizeof(XmArena));
    if (!func->arena) {
        xr_free(func);
        return NULL;
    }
    xm_arena_init(func->arena);

    func->name = name;

    // Pre-allocate arrays
    func->blocks = (XmBlock **) xr_calloc(INIT_BLOCKS, sizeof(XmBlock *));
    func->vregs = (XmVReg *) xr_calloc(INIT_VREGS, sizeof(XmVReg));
    func->consts = (XmConst *) xr_calloc(INIT_CONSTS, sizeof(XmConst));
    if (!func->blocks || !func->vregs || !func->consts) {
        xm_func_destroy(func);
        return NULL;
    }
    func->blk_cap = INIT_BLOCKS;
    func->nblk = 0;
    func->vreg_cap = INIT_VREGS;
    func->nvreg = 0;
    func->const_cap = INIT_CONSTS;
    func->nconst = 0;
    func->const_ht = NULL;
    func->const_ht_mask = 0;

    // Call arg pool (lazy-initialized on first use)
    func->call_arg_pool = NULL;
    func->call_arg_pool_used = 0;
    func->call_arg_pool_cap = 0;

    return func;
}

void xm_func_destroy(XmFunc *func) {
    if (!func)
        return;
    if (func->arena) {
        xm_arena_destroy(func->arena);
        xr_free(func->arena);
    }
    xr_free(func->deopt_infos);
    xm_func_invalidate_loops(func);  // also invalidates the dom-tree
    xm_func_invalidate_defuse(func);
    xm_func_invalidate_alias(func);
    xr_free(func->blocks);
    xr_free(func->vregs);
    xr_free(func->consts);
    xr_free(func->const_ht);
    xr_free(func->call_arg_pool);
    xr_free(func);
}

/* ========== Blocks ========== */

XmBlock *xm_func_add_block(XmFunc *func, const char *label) {
    XR_DCHECK(func != NULL, "xm_func_add_block: NULL func");
    XR_DCHECK(func->arena != NULL, "xm_func_add_block: NULL arena");
    XmBlock *blk = (XmBlock *) xm_arena_calloc(func->arena, 1, sizeof(XmBlock));
    if (!blk)
        return NULL;

    blk->id = func->nblk;
    blk->label = label ? (char *) label : NULL;
    blk->jmp.type = XM_JMP_NONE;
    blk->jmp.arg = XM_NONE;
    blk->rpo_id = (uint32_t) -1;

    // Instruction array (arena-allocated, can grow)
    blk->ins = (XmIns *) xm_arena_alloc(func->arena, INIT_INS * sizeof(XmIns));
    blk->ins_cap = INIT_INS;
    blk->nins = 0;

    // Predecessor array
    blk->preds = (XmBlock **) xm_arena_alloc(func->arena, INIT_PREDS * sizeof(XmBlock *));
    blk->pred_cap = INIT_PREDS;
    blk->npred = 0;

    // Add to func
    if (func->nblk >= func->blk_cap) {
        uint32_t new_cap = func->blk_cap * 2;
        XR_REALLOC_OR_ABORT(func->blocks, new_cap * sizeof(XmBlock *), "xm: block array");
        func->blk_cap = new_cap;
    }
    func->blocks[func->nblk++] = blk;

    if (!func->entry) {
        func->entry = blk;
    }

    return blk;
}

void xm_block_add_pred(XmBlock *blk, XmBlock *pred, XmArena *arena) {
    XR_DCHECK(blk != NULL, "xm_block_add_pred: NULL blk");
    XR_DCHECK(pred != NULL, "xm_block_add_pred: NULL pred");
    XR_DCHECK(arena != NULL, "xm_block_add_pred: NULL arena");
    if (blk->npred >= blk->pred_cap) {
        uint32_t new_cap = blk->pred_cap * 2;
        XmBlock **new_preds = (XmBlock **) xm_arena_alloc(arena, new_cap * sizeof(XmBlock *));
        if (!new_preds)
            return;
        memcpy(new_preds, blk->preds, blk->npred * sizeof(XmBlock *));
        blk->preds = new_preds;
        blk->pred_cap = new_cap;
    }
    blk->preds[blk->npred++] = pred;
}

void xm_block_set_jmp(XmBlock *blk, XmBlock *target) {
    XR_DCHECK(blk != NULL, "xm_block_set_jmp: NULL blk");
    XR_DCHECK(target != NULL, "xm_block_set_jmp: NULL target");
    blk->jmp.type = XM_JMP_JMP;
    blk->jmp.arg = XM_NONE;
    blk->s1 = target;
    blk->s2 = NULL;
}

void xm_block_set_br(XmBlock *blk, XmRef cond, XmBlock *if_true, XmBlock *if_false) {
    XR_DCHECK(blk != NULL, "xm_block_set_br: NULL blk");
    XR_DCHECK(if_true != NULL, "xm_block_set_br: NULL if_true");
    XR_DCHECK(if_false != NULL, "xm_block_set_br: NULL if_false");
    blk->jmp.type = XM_JMP_BR;
    blk->jmp.arg = cond;
    blk->s1 = if_true;
    blk->s2 = if_false;
}

void xm_block_set_ret(XmBlock *blk, XmRef val) {
    XR_DCHECK(blk != NULL, "xm_block_set_ret: NULL blk");
    blk->jmp.type = XM_JMP_RET;
    blk->jmp.arg = val;
    blk->s1 = NULL;
    blk->s2 = NULL;
}

/* ========== Instruction Emit ========== */

// Insert a zeroed instruction slot at position `pos` in block.
// Shifts subsequent instructions down. Returns pointer to new slot, or NULL on failure.
XmIns *xm_block_insert_at(XmFunc *func, XmBlock *blk, uint32_t pos) {
    if (pos > blk->nins)
        return NULL;
    // Ensure capacity
    if (blk->nins >= blk->ins_cap) {
        uint32_t new_cap = blk->ins_cap * 2;
        XmIns *new_ins = (XmIns *) xm_arena_alloc(func->arena, new_cap * sizeof(XmIns));
        if (!new_ins)
            return NULL;
        memcpy(new_ins, blk->ins, blk->nins * sizeof(XmIns));
        blk->ins = new_ins;
        blk->ins_cap = new_cap;
    }
    // Shift instructions after pos
    if (pos < blk->nins) {
        memmove(&blk->ins[pos + 1], &blk->ins[pos], (blk->nins - pos) * sizeof(XmIns));
    }
    blk->nins++;
    // Zero the new slot
    memset(&blk->ins[pos], 0, sizeof(XmIns));
    // Update vreg->def pointers for shifted instructions
    for (uint32_t i = pos + 1; i < blk->nins; i++) {
        XmIns *ins = &blk->ins[i];
        if (xm_ref_is_vreg(ins->dst)) {
            uint32_t vi = XM_REF_INDEX(ins->dst);
            if (vi < func->nvreg)
                func->vregs[vi].def = ins;
        }
    }
    return &blk->ins[pos];
}

// Grow block instruction array if needed
static XmIns *block_grow_ins(XmFunc *func, XmBlock *blk) {
    if (blk->nins >= blk->ins_cap) {
        uint32_t new_cap = blk->ins_cap * 2;
        XmIns *new_ins = (XmIns *) xm_arena_alloc(func->arena, new_cap * sizeof(XmIns));
        if (!new_ins)
            return NULL;
        memcpy(new_ins, blk->ins, blk->nins * sizeof(XmIns));
        blk->ins = new_ins;
        blk->ins_cap = new_cap;
    }
    return &blk->ins[blk->nins];
}

// Grow vreg array if needed
static void func_grow_vregs(XmFunc *func) {
    if (func->nvreg >= func->vreg_cap) {
        uint32_t old_cap = func->vreg_cap;
        uint32_t new_cap = old_cap * 2;
        XR_REALLOC_OR_ABORT(func->vregs, new_cap * sizeof(XmVReg), "xm func_grow_vregs");
        memset(func->vregs + old_cap, 0, (new_cap - old_cap) * sizeof(XmVReg));
        func->vreg_cap = new_cap;
    }
}

XmRef xm_new_vreg(XmFunc *func, uint8_t type) {
    XR_DCHECK(func != NULL, "xm_new_vreg: NULL func");
    func_grow_vregs(func);
    uint32_t idx = func->nvreg++;
    func->vregs[idx].rep = type;
    func->vregs[idx].heap_type = 0;
    func->vregs[idx].bc_slot = -1;
    func->vregs[idx].struct_idx = -1;
    func->vregs[idx].reg = -1;
    func->vregs[idx].def = NULL;
    func->vregs[idx].callee_proto = NULL;
    func->vregs[idx].shape_hint = NULL;
    func->vregs[idx].layout = NULL;
    func->vregs[idx].array_etype = 0xFF;
    func->vregs[idx].array_ecount = 0;
    func->vregs[idx].is_fresh_alloc = false;
    return XM_REF(XM_REF_VREG, idx);
}

XmRef xm_emit(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a, XmRef b) {
    XR_DCHECK(func != NULL, "xm_emit: NULL func");
    XR_DCHECK(blk != NULL, "xm_emit: NULL blk");
    XmIns *ins = block_grow_ins(func, blk);
    if (!ins)
        return XM_NONE;

    XmRef dst = (type != XR_REP_VOID) ? xm_new_vreg(func, type) : XM_NONE;

    ins->op = op;
    ins->rep = type;
    ins->flags = 0;
    if (xm_op_has_side_effect(op))
        ins->flags |= XM_FLAG_SIDE_EFFECT;
    if (xm_op_is_commutative(op))
        ins->flags |= XM_FLAG_COMMUTATIVE;
    // ctype starts UNKNOWN: rep is for register allocation only.
    // Builder handlers explicitly set ctype via builder_tag_vreg().
    ins->ctype = XM_TYPE_UNKNOWN;
    ins->dst = dst;
    ins->args[0] = a;
    ins->args[1] = b;
    blk->nins++;

    // Link vreg to defining instruction
    if (!xm_ref_is_none(dst)) {
        uint32_t di = XM_REF_INDEX(dst);
        func->vregs[di].def = ins;
    }

    return dst;
}

XmRef xm_emit_unary(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a) {
    return xm_emit(func, blk, op, type, a, XM_NONE);
}

void xm_emit_void(XmFunc *func, XmBlock *blk, uint16_t op, XmRef a, XmRef b) {
    XR_DCHECK(func != NULL, "xm_emit_void: NULL func");
    XR_DCHECK(blk != NULL, "xm_emit_void: NULL blk");
    XmIns *ins = block_grow_ins(func, blk);
    if (!ins)
        return;

    ins->op = op;
    ins->rep = XR_REP_VOID;
    ins->flags = 0;
    if (xm_op_has_side_effect(op))
        ins->flags |= XM_FLAG_SIDE_EFFECT;
    if (xm_op_is_commutative(op))
        ins->flags |= XM_FLAG_COMMUTATIVE;
    ins->ctype = XM_TYPE_UNKNOWN;
    ins->dst = XM_NONE;
    ins->args[0] = a;
    ins->args[1] = b;
    blk->nins++;
}

void xm_emit_raw(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef dst, XmRef arg0,
                 XmRef arg1) {
    XR_DCHECK(func != NULL, "xm_emit_raw: NULL func");
    XR_DCHECK(blk != NULL, "xm_emit_raw: NULL blk");
    XmIns *ins = block_grow_ins(func, blk);
    if (!ins)
        return;

    ins->op = op;
    ins->rep = type;
    ins->flags = 0;
    if (xm_op_has_side_effect(op))
        ins->flags |= XM_FLAG_SIDE_EFFECT;
    if (xm_op_is_commutative(op))
        ins->flags |= XM_FLAG_COMMUTATIVE;
    ins->ctype = XM_TYPE_UNKNOWN;
    ins->dst = dst;
    ins->args[0] = arg0;
    ins->args[1] = arg1;
    blk->nins++;
}

/* ========== Call Arg Pool ========== */

#define INIT_CALL_ARG_POOL 64

uint32_t xm_func_add_call_args(XmFunc *func, const XmRef *args, uint16_t nargs) {
    XR_DCHECK(func != NULL, "xm_func_add_call_args: NULL func");
    if (nargs == 0)
        return 0;

    // Lazy init
    if (!func->call_arg_pool) {
        uint32_t cap = (nargs > INIT_CALL_ARG_POOL) ? nargs : INIT_CALL_ARG_POOL;
        func->call_arg_pool = (XmRef *) xr_calloc(cap, sizeof(XmRef));
        if (!func->call_arg_pool)
            return 0;
        func->call_arg_pool_cap = cap;
        func->call_arg_pool_used = 0;
    }

    // Grow if needed
    while (func->call_arg_pool_used + nargs > func->call_arg_pool_cap) {
        uint32_t new_cap = func->call_arg_pool_cap * 2;
        XR_REALLOC_OR_ABORT(func->call_arg_pool, new_cap * sizeof(XmRef), "xm: call arg pool");
        func->call_arg_pool_cap = new_cap;
    }

    uint32_t start = func->call_arg_pool_used;
    memcpy(&func->call_arg_pool[start], args, nargs * sizeof(XmRef));
    func->call_arg_pool_used += nargs;
    return start;
}

void xm_func_bind_call_args(XmFunc *func, XmRef dst, const XmRef *args, uint16_t nargs) {
    if (!xm_ref_is_vreg(dst) || nargs == 0)
        return;
    uint32_t vi = XM_REF_INDEX(dst);
    if (vi >= func->nvreg)
        return;
    uint32_t start = xm_func_add_call_args(func, args, nargs);
    func->vregs[vi].call_arg_start = start;
    func->vregs[vi].call_nargs = nargs;
}

/* ========== Constants ========== */

// Hash function for constant dedup: mix type byte with raw value bits
static inline uint32_t const_hash(uint8_t type, uint64_t raw) {
    uint64_t h = raw ^ ((uint64_t) type * 0x9E3779B97F4A7C15ULL);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (uint32_t) h;
}

#define CONST_HT_INIT_SIZE 128  // must be power of 2
#define CONST_HT_LOAD_NUM 3     // grow when nconst * 4 > table_size * 3 (75%)
#define CONST_HT_LOAD_DEN 4

// Rebuild hash table after resize
static void const_ht_rebuild(XmFunc *func) {
    uint32_t size = func->const_ht_mask + 1;
    memset(func->const_ht, 0, size * sizeof(uint32_t));
    for (uint32_t i = 0; i < func->nconst; i++) {
        uint32_t h = const_hash(func->consts[i].rep, func->consts[i].val.raw) & func->const_ht_mask;
        while (func->const_ht[h] != 0)
            h = (h + 1) & func->const_ht_mask;
        func->const_ht[h] = i + 1;  // store index+1 (0 = empty)
    }
}

static XmRef add_const(XmFunc *func, XmConstVal val, uint8_t type) {
    // Initialize hash table on first constant
    if (!func->const_ht) {
        func->const_ht = (uint32_t *) xr_calloc(CONST_HT_INIT_SIZE, sizeof(uint32_t));
        if (!func->const_ht)
            goto fallback_linear;
        func->const_ht_mask = CONST_HT_INIT_SIZE - 1;
    }

    // Hash-based dedup lookup
    {
        uint32_t h = const_hash(type, val.raw) & func->const_ht_mask;
        while (func->const_ht[h] != 0) {
            uint32_t ci = func->const_ht[h] - 1;
            if (func->consts[ci].rep == type && func->consts[ci].val.raw == val.raw)
                return XM_REF(XM_REF_CONST, ci);
            h = (h + 1) & func->const_ht_mask;
        }
    }

    // Not found — insert new constant
    if (func->nconst >= func->const_cap) {
        uint32_t new_cap = func->const_cap * 2;
        XR_REALLOC_OR_ABORT(func->consts, new_cap * sizeof(XmConst), "xm: const pool");
        func->const_cap = new_cap;
    }

    uint32_t idx = func->nconst++;
    func->consts[idx].val = val;
    func->consts[idx].rep = type;

    // Insert into hash table; grow if load factor exceeded
    uint32_t ht_size = func->const_ht_mask + 1;
    if (func->nconst * CONST_HT_LOAD_DEN > ht_size * CONST_HT_LOAD_NUM) {
        uint32_t new_size = ht_size * 2;
        uint32_t *new_ht = (uint32_t *) xr_calloc(new_size, sizeof(uint32_t));
        if (new_ht) {
            xr_free(func->const_ht);
            func->const_ht = new_ht;
            func->const_ht_mask = new_size - 1;
        }
        const_ht_rebuild(func);
    } else {
        uint32_t h = const_hash(type, val.raw) & func->const_ht_mask;
        while (func->const_ht[h] != 0)
            h = (h + 1) & func->const_ht_mask;
        func->const_ht[h] = idx + 1;
    }
    return XM_REF(XM_REF_CONST, idx);

fallback_linear:
    // Fallback: linear scan if hash table allocation failed
    for (uint32_t i = 0; i < func->nconst; i++) {
        if (func->consts[i].rep == type && func->consts[i].val.raw == val.raw)
            return XM_REF(XM_REF_CONST, i);
    }
    if (func->nconst >= func->const_cap) {
        uint32_t new_cap = func->const_cap * 2;
        XR_REALLOC_OR_ABORT(func->consts, new_cap * sizeof(XmConst), "xm: const pool");
        func->const_cap = new_cap;
    }
    uint32_t fi = func->nconst++;
    func->consts[fi].val = val;
    func->consts[fi].rep = type;
    return XM_REF(XM_REF_CONST, fi);
}

XmRef xm_const_i64(XmFunc *func, int64_t val) {
    XmConstVal cv = {.i64 = val};
    return add_const(func, cv, XR_REP_I64);
}

XmRef xm_const_f64(XmFunc *func, double val) {
    XmConstVal cv = {.f64 = val};
    return add_const(func, cv, XR_REP_F64);
}

XmRef xm_const_ptr(XmFunc *func, void *val) {
    XmConstVal cv = {.ptr = val};
    return add_const(func, cv, XR_REP_PTR);
}

XmRef xm_const_string(XmFunc *func, const char *chars, uint32_t len) {
    XmConstVal cv;
    cv.str.chars = chars;
    cv.str.len = len;
    return add_const(func, cv, XR_REP_STR);
}

/* ========== Phi Nodes ========== */

XmPhi *xm_add_phi(XmFunc *func, XmBlock *blk, uint8_t type) {
    XmPhi *phi = (XmPhi *) xm_arena_calloc(func->arena, 1, sizeof(XmPhi));
    if (!phi)
        return NULL;

    phi->dst = xm_new_vreg(func, type);
    phi->rep = type;
    switch (type) {
        case XR_REP_I64:
            phi->ctype = (XmType) {XM_TK_INT, 0, 0};
            break;
        case XR_REP_F64:
            phi->ctype = (XmType) {XM_TK_FLOAT, 0, 0};
            break;
        case XR_REP_PTR:
            phi->ctype = (XmType) {XM_TK_PTR, 0, 0};
            break;
        default:
            phi->ctype = XM_TYPE_UNKNOWN;
            break;
    }
    phi->narg = blk->npred;
    phi->args = (XmRef *) xm_arena_calloc(func->arena, blk->npred, sizeof(XmRef));

    // Initialize all args to XM_NONE
    for (uint16_t i = 0; i < phi->narg; i++) {
        phi->args[i] = XM_NONE;
    }

    // Prepend to block's phi list
    phi->next = blk->phis;
    blk->phis = phi;

    return phi;
}

void xm_phi_set_arg(XmPhi *phi, uint32_t pred_idx, XmRef val) {
    XR_DCHECK(pred_idx < phi->narg, "xm_phi_set_arg: pred_idx out of range");
    phi->args[pred_idx] = val;
}

/* ========== Opcode Info ========== */

static const char *op_names[] = {
    [XM_ADD] = "add",
    [XM_SUB] = "sub",
    [XM_MUL] = "mul",
    [XM_DIV] = "div",
    [XM_MOD] = "mod",
    [XM_NEG] = "neg",
    [XM_AND] = "and",
    [XM_OR] = "or",
    [XM_XOR] = "xor",
    [XM_NOT] = "not",
    [XM_SHL] = "shl",
    [XM_SHR] = "shr",
    [XM_FADD] = "fadd",
    [XM_FSUB] = "fsub",
    [XM_FMUL] = "fmul",
    [XM_FDIV] = "fdiv",
    [XM_FNEG] = "fneg",
    [XM_I2F] = "i2f",
    [XM_F2I] = "f2i",
    [XM_EQ] = "eq",
    [XM_NE] = "ne",
    [XM_LT] = "lt",
    [XM_LE] = "le",
    [XM_GT] = "gt",
    [XM_GE] = "ge",
    [XM_FEQ] = "feq",
    [XM_FNE] = "fne",
    [XM_FLT] = "flt",
    [XM_FLE] = "fle",
    [XM_BOX_I64] = "box.i64",
    [XM_BOX_F64] = "box.f64",
    [XM_UNBOX_I64] = "unbox.i64",
    [XM_UNBOX_F64] = "unbox.f64",
    [XM_TAG_CHECK] = "tag.check",
    [XM_TAG_LOAD] = "tag.load",
    [XM_LOAD] = "load",
    [XM_STORE] = "store",
    [XM_LOAD_FIELD] = "load.field",
    [XM_STORE_FIELD] = "store.field",
    [XM_ALLOC] = "alloc",
    [XM_STORE_CORO] = "store_coro",
    [XM_STORE_CORO_BYTE] = "store_coro.b",
    [XM_LOAD_CORO] = "load_coro",
    [XM_LOAD_CORO_BYTE] = "load_coro.b",
    [XM_LOAD32S] = "load32s",
    [XM_LOAD8Z] = "load8z",
    [XM_LOAD8S] = "load8s",
    [XM_STORE8] = "store8",
    [XM_LOAD16Z] = "load16z",
    [XM_LOAD16S] = "load16s",
    [XM_STORE16] = "store16",
    [XM_LOAD32Z] = "load32z",
    [XM_STORE32] = "store32",
    [XM_LOAD_F32] = "load.f32",
    [XM_STORE_F32] = "store.f32",
    [XM_GUARD_BOUNDS] = "guard.bounds",
    [XM_CONST_I64] = "const.i64",
    [XM_CONST_F64] = "const.f64",
    [XM_CONST_PTR] = "const.ptr",
    [XM_JMP] = "jmp",
    [XM_BR] = "br",
    [XM_RET] = "ret",
    [XM_CALL] = "call",
    [XM_CALL_C] = "call.c",
    [XM_CALL_C_LEAF] = "call.c.leaf",
    [XM_CALL_SELF_DIRECT] = "call.self.direct",
    [XM_CALL_KNOWN_REG] = "call.known.reg",
    [XM_CALL_INTRINSIC] = "call.intrinsic",
    [XM_RT_ADD] = "rt.add",
    [XM_RT_SUB] = "rt.sub",
    [XM_RT_MUL] = "rt.mul",
    [XM_RT_DIV] = "rt.div",
    [XM_RT_MOD] = "rt.mod",
    [XM_RT_UNM] = "rt.unm",
    [XM_RT_LT] = "rt.lt",
    [XM_RT_LE] = "rt.le",
    [XM_RT_EQ] = "rt.eq",
    [XM_RT_PRINT] = "rt.print",
    [XM_RT_ARRAY_NEW] = "rt.array_new",
    [XM_RT_ARRAY_PUSH] = "rt.array_push",
    [XM_RT_ARRAY_LEN] = "rt.array_len",
    [XM_RT_INDEX_GET] = "rt.index_get",
    [XM_RT_INDEX_SET] = "rt.index_set",
    [XM_RT_MAP_NEW] = "rt.map_new",
    [XM_RT_ISNULL] = "rt.isnull",
    [XM_SAFEPOINT] = "safepoint",
    [XM_BARRIER_FWD] = "barrier.fwd",
    [XM_BARRIER_BACK] = "barrier.back",
    [XM_DEOPT] = "deopt",
    [XM_GUARD_TAG] = "guard.tag",
    [XM_GUARD_CLASS] = "guard.class",
    [XM_GUARD_KLASS] = "guard.klass",
    [XM_GUARD_NONNULL] = "guard.nonnull",
    [XM_GUARD_SHAPE] = "guard.shape",
    [XM_TRY_BEGIN] = "try.begin",
    [XM_TRY_END] = "try.end",
    [XM_THROW] = "throw",
    [XM_CATCH] = "catch",
    [XM_RETAIN] = "arc.retain",
    [XM_RELEASE] = "arc.release",
    [XM_LOAD_UPVAL] = "load.upval",
    [XM_STORE_UPVAL] = "store.upval",
    [XM_MOV] = "mov",
    [XM_NOP] = "nop",
    [XM_PHI] = "phi",
    [XM_REDEFINE] = "redefine",
};

const char *xm_op_name(uint16_t op) {
    if (op < XM_OP_COUNT && op_names[op]) {
        return op_names[op];
    }
    if (op >= XM_MACH_BASE) {
        return "mach.???";
    }
    return "???";
}

bool xm_op_is_commutative(uint16_t op) {
    switch (op) {
        case XM_ADD:
        case XM_MUL:
        case XM_AND:
        case XM_OR:
        case XM_XOR:
        case XM_FADD:
        case XM_FMUL:
        case XM_EQ:
        case XM_NE:
        case XM_FEQ:
        case XM_FNE:
        case XM_RT_ADD:
        case XM_RT_MUL:
        case XM_RT_EQ:
            return true;
        default:
            return false;
    }
}

bool xm_op_is_pure(uint16_t op) {
    // Pure ops: arithmetic, logic, comparison, constants, REDEFINE
    return op <= XM_FLE || op == XM_CONST_I64 || op == XM_CONST_F64 || op == XM_REDEFINE;
}

/* ========== Dominator Utilities ========== */

// Compute immediate dominators using Cooper's algorithm.
// Requires blocks[0] = entry. Uses block->id as RPO approximation
// (valid for Xm's structured CFG from builder).
void xm_compute_idom(XmFunc *func, uint32_t *idom, uint32_t nblk) {
    for (uint32_t i = 0; i < nblk; i++)
        idom[i] = UINT32_MAX;
    idom[0] = 0;

    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = 1; bi < nblk; bi++) {
            XmBlock *blk = func->blocks[bi];
            uint32_t new_idom = UINT32_MAX;
            for (uint32_t p = 0; p < blk->npred; p++) {
                uint32_t pid = blk->preds[p]->id;
                if (pid >= nblk || idom[pid] == UINT32_MAX)
                    continue;
                if (new_idom == UINT32_MAX) {
                    new_idom = pid;
                } else {
                    // intersect
                    uint32_t a = new_idom, b = pid;
                    while (a != b) {
                        while (a > b)
                            a = idom[a];
                        while (b > a)
                            b = idom[b];
                    }
                    new_idom = a;
                }
            }
            if (new_idom != UINT32_MAX && new_idom != idom[bi]) {
                idom[bi] = new_idom;
                changed = true;
            }
        }
    }
}

void xm_rebuild_vreg_defs(XmFunc *func) {
    if (!func)
        return;
    // Clear all def pointers first
    for (uint32_t v = 0; v < func->nvreg; v++)
        func->vregs[v].def = NULL;
    // Rescan all instructions and set def pointers
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t v = XM_REF_INDEX(ins->dst);
                if (v < func->nvreg)
                    func->vregs[v].def = ins;
            }
        }
    }
}

bool xm_op_has_side_effect(uint16_t op) {
    switch (op) {
        // Memory stores
        case XM_STORE:
        case XM_STORE_FIELD:
        case XM_STORE8:
        case XM_STORE16:
        case XM_STORE32:
        case XM_STORE_F32:
        case XM_STORE_CORO:
        case XM_STORE_CORO_BYTE:
        case XM_STORE_UPVAL:
        // Calls
        case XM_CALL:
        case XM_CALL_C:
        case XM_CALL_C_LEAF:
        case XM_CALL_SELF_DIRECT:
        case XM_CALL_DIRECT:
        case XM_CALL_KNOWN:
        case XM_CALL_KNOWN_REG:
        case XM_CALL_INTRINSIC:
        // Mixed-type runtime helpers (may allocate, throw, or have observable effects)
        case XM_RT_ADD:
        case XM_RT_SUB:
        case XM_RT_MUL:
        case XM_RT_DIV:
        case XM_RT_MOD:
        case XM_RT_UNM:
        case XM_RT_LT:
        case XM_RT_LE:
        case XM_RT_EQ:
        case XM_RT_PRINT:
        case XM_RT_ARRAY_NEW:
        case XM_RT_ARRAY_PUSH:
        case XM_RT_ARRAY_LEN:
        case XM_RT_INDEX_GET:
        case XM_RT_INDEX_SET:
        case XM_RT_MAP_NEW:
        case XM_RT_ISNULL:
        // GC / barriers
        case XM_SAFEPOINT:
        case XM_BARRIER_FWD:
        case XM_BARRIER_BACK:
        case XM_RETAIN:
        case XM_RELEASE:
        case XM_ALLOC:
        // Guards (may deopt — observable side effect)
        case XM_DEOPT:
        case XM_GUARD_TAG:
        case XM_GUARD_CLASS:
        case XM_GUARD_KLASS:
        case XM_GUARD_NONNULL:
        case XM_GUARD_SHAPE:
        case XM_GUARD_BOUNDS:
        // Exception handling
        case XM_THROW:
        case XM_TRY_BEGIN:
        case XM_TRY_END:
        case XM_CATCH:
            return true;
        default:
            return false;
    }
}
