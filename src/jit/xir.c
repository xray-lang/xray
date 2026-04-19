/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir.c - XIR core data structures and operations
 *
 * KEY CONCEPT:
 *   Arena-based allocation for all IR nodes. Everything in a XirFunc
 *   is allocated from its arena and freed in one shot.
 */

#include "xir.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Arena Allocator ========== */

static XirArenaPage *arena_new_page(size_t min_size) {
    size_t page_size = min_size < XIR_ARENA_PAGE_SIZE ? XIR_ARENA_PAGE_SIZE : min_size;
    XirArenaPage *page = (XirArenaPage *)xr_malloc(sizeof(XirArenaPage) + page_size);
    if (!page) return NULL;
    page->next = NULL;
    page->used = 0;
    page->size = page_size;
    return page;
}

void xir_arena_init(XirArena *arena) {
    XR_DCHECK(arena != NULL, "xir_arena_init: NULL arena");
    arena->current = NULL;
    arena->pages = NULL;
    arena->total_allocated = 0;
}

void xir_arena_destroy(XirArena *arena) {
    XirArenaPage *page = arena->pages;
    while (page) {
        XirArenaPage *next = page->next;
        xr_free(page);
        page = next;
    }
    arena->current = NULL;
    arena->pages = NULL;
    arena->total_allocated = 0;
}

void *xir_arena_alloc(XirArena *arena, size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~(size_t)7;

    if (arena->current && arena->current->used + size <= arena->current->size) {
        void *ptr = arena->current->data + arena->current->used;
        arena->current->used += size;
        return ptr;
    }

    // Need new page
    XirArenaPage *page = arena_new_page(size + sizeof(XirArenaPage));
    if (!page) return NULL;

    page->next = arena->pages;
    arena->pages = page;
    arena->current = page;
    arena->total_allocated += page->size;

    void *ptr = page->data;
    page->used = size;
    return ptr;
}

/* ========== XirFunc ========== */

#define INIT_BLOCKS  16
#define INIT_VREGS   64
#define INIT_CONSTS  32
#define INIT_INS     32
#define INIT_PREDS   4

XirFunc *xir_func_new(const char *name) {
    XirFunc *func = (XirFunc *)xr_calloc(1, sizeof(XirFunc));
    if (!func) return NULL;

    func->arena = (XirArena *)xr_malloc(sizeof(XirArena));
    if (!func->arena) { xr_free(func); return NULL; }
    xir_arena_init(func->arena);

    func->name = name;

    // Pre-allocate arrays
    func->blocks = (XirBlock **)xr_calloc(INIT_BLOCKS, sizeof(XirBlock *));
    func->vregs = (XirVReg *)xr_calloc(INIT_VREGS, sizeof(XirVReg));
    func->consts = (XirConst *)xr_calloc(INIT_CONSTS, sizeof(XirConst));
    if (!func->blocks || !func->vregs || !func->consts) {
        xir_func_destroy(func);
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

void xir_func_destroy(XirFunc *func) {
    if (!func) return;
    if (func->arena) {
        xir_arena_destroy(func->arena);
        xr_free(func->arena);
    }
    xr_free(func->deopt_infos);
    xr_free(func->idom);
    xr_free(func->blocks);
    xr_free(func->vregs);
    xr_free(func->consts);
    xr_free(func->const_ht);
    xr_free(func->call_arg_pool);
    xr_free(func);
}

/* ========== Blocks ========== */

XirBlock *xir_func_add_block(XirFunc *func, const char *label) {
    XR_DCHECK(func != NULL, "xir_func_add_block: NULL func");
    XR_DCHECK(func->arena != NULL, "xir_func_add_block: NULL arena");
    XirBlock *blk = (XirBlock *)xir_arena_calloc(func->arena, 1, sizeof(XirBlock));
    if (!blk) return NULL;

    blk->id = func->nblk;
    blk->label = label ? (char *)label : NULL;
    blk->jmp.type = XIR_JMP_NONE;
    blk->jmp.arg = XIR_NONE;
    blk->rpo_id = (uint32_t)-1;

    // Instruction array (arena-allocated, can grow)
    blk->ins = (XirIns *)xir_arena_alloc(func->arena, INIT_INS * sizeof(XirIns));
    blk->ins_cap = INIT_INS;
    blk->nins = 0;

    // Predecessor array
    blk->preds = (XirBlock **)xir_arena_alloc(func->arena, INIT_PREDS * sizeof(XirBlock *));
    blk->pred_cap = INIT_PREDS;
    blk->npred = 0;

    // Add to func
    if (func->nblk >= func->blk_cap) {
        uint32_t new_cap = func->blk_cap * 2;
        if (!XR_REALLOC(func->blocks, new_cap * sizeof(XirBlock *)))
            return NULL;
        func->blk_cap = new_cap;
    }
    func->blocks[func->nblk++] = blk;

    if (!func->entry) {
        func->entry = blk;
    }

    return blk;
}

void xir_block_add_pred(XirBlock *blk, XirBlock *pred, XirArena *arena) {
    XR_DCHECK(blk != NULL, "xir_block_add_pred: NULL blk");
    XR_DCHECK(pred != NULL, "xir_block_add_pred: NULL pred");
    XR_DCHECK(arena != NULL, "xir_block_add_pred: NULL arena");
    if (blk->npred >= blk->pred_cap) {
        uint32_t new_cap = blk->pred_cap * 2;
        XirBlock **new_preds = (XirBlock **)xir_arena_alloc(arena, new_cap * sizeof(XirBlock *));
        if (!new_preds) return;
        memcpy(new_preds, blk->preds, blk->npred * sizeof(XirBlock *));
        blk->preds = new_preds;
        blk->pred_cap = new_cap;
    }
    blk->preds[blk->npred++] = pred;
}

void xir_block_set_jmp(XirBlock *blk, XirBlock *target) {
    XR_DCHECK(blk != NULL, "xir_block_set_jmp: NULL blk");
    XR_DCHECK(target != NULL, "xir_block_set_jmp: NULL target");
    blk->jmp.type = XIR_JMP_JMP;
    blk->jmp.arg = XIR_NONE;
    blk->s1 = target;
    blk->s2 = NULL;
}

void xir_block_set_br(XirBlock *blk, XirRef cond, XirBlock *if_true, XirBlock *if_false) {
    XR_DCHECK(blk != NULL, "xir_block_set_br: NULL blk");
    XR_DCHECK(if_true != NULL, "xir_block_set_br: NULL if_true");
    XR_DCHECK(if_false != NULL, "xir_block_set_br: NULL if_false");
    blk->jmp.type = XIR_JMP_BR;
    blk->jmp.arg = cond;
    blk->s1 = if_true;
    blk->s2 = if_false;
}

void xir_block_set_ret(XirBlock *blk, XirRef val) {
    XR_DCHECK(blk != NULL, "xir_block_set_ret: NULL blk");
    blk->jmp.type = XIR_JMP_RET;
    blk->jmp.arg = val;
    blk->s1 = NULL;
    blk->s2 = NULL;
}

/* ========== Instruction Emit ========== */

// Insert a zeroed instruction slot at position `pos` in block.
// Shifts subsequent instructions down. Returns pointer to new slot, or NULL on failure.
XirIns *xir_block_insert_at(XirFunc *func, XirBlock *blk, uint32_t pos) {
    if (pos > blk->nins) return NULL;
    // Ensure capacity
    if (blk->nins >= blk->ins_cap) {
        uint32_t new_cap = blk->ins_cap * 2;
        XirIns *new_ins = (XirIns *)xir_arena_alloc(func->arena, new_cap * sizeof(XirIns));
        if (!new_ins) return NULL;
        memcpy(new_ins, blk->ins, blk->nins * sizeof(XirIns));
        blk->ins = new_ins;
        blk->ins_cap = new_cap;
    }
    // Shift instructions after pos
    if (pos < blk->nins) {
        memmove(&blk->ins[pos + 1], &blk->ins[pos],
                (blk->nins - pos) * sizeof(XirIns));
    }
    blk->nins++;
    // Zero the new slot
    memset(&blk->ins[pos], 0, sizeof(XirIns));
    // Update vreg->def pointers for shifted instructions
    for (uint32_t i = pos + 1; i < blk->nins; i++) {
        XirIns *ins = &blk->ins[i];
        if (xir_ref_is_vreg(ins->dst)) {
            uint32_t vi = XIR_REF_INDEX(ins->dst);
            if (vi < func->nvreg)
                func->vregs[vi].def = ins;
        }
    }
    return &blk->ins[pos];
}

// Grow block instruction array if needed
static XirIns *block_grow_ins(XirFunc *func, XirBlock *blk) {
    if (blk->nins >= blk->ins_cap) {
        uint32_t new_cap = blk->ins_cap * 2;
        XirIns *new_ins = (XirIns *)xir_arena_alloc(func->arena, new_cap * sizeof(XirIns));
        if (!new_ins) return NULL;
        memcpy(new_ins, blk->ins, blk->nins * sizeof(XirIns));
        blk->ins = new_ins;
        blk->ins_cap = new_cap;
    }
    return &blk->ins[blk->nins];
}

// Grow vreg array if needed
static void func_grow_vregs(XirFunc *func) {
    if (func->nvreg >= func->vreg_cap) {
        uint32_t old_cap = func->vreg_cap;
        uint32_t new_cap = old_cap * 2;
        XR_REALLOC_OR_ABORT(func->vregs, new_cap * sizeof(XirVReg),
                            "xir func_grow_vregs");
        memset(func->vregs + old_cap, 0, (new_cap - old_cap) * sizeof(XirVReg));
        func->vreg_cap = new_cap;
    }
}

XirRef xir_new_vreg(XirFunc *func, uint8_t type) {
    XR_DCHECK(func != NULL, "xir_new_vreg: NULL func");
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
    return XIR_REF(XIR_REF_VREG, idx);
}

XirRef xir_emit(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type, XirRef a, XirRef b) {
    XR_DCHECK(func != NULL, "xir_emit: NULL func");
    XR_DCHECK(blk != NULL, "xir_emit: NULL blk");
    XirIns *ins = block_grow_ins(func, blk);
    if (!ins) return XIR_NONE;

    XirRef dst = (type != XR_REP_VOID) ? xir_new_vreg(func, type) : XIR_NONE;

    ins->op = op;
    ins->rep = type;
    ins->flags = 0;
    if (xir_op_has_side_effect(op))  ins->flags |= XIR_FLAG_SIDE_EFFECT;
    if (xir_op_is_commutative(op))   ins->flags |= XIR_FLAG_COMMUTATIVE;
    // ctype starts UNKNOWN: rep is for register allocation only.
    // Builder handlers explicitly set ctype via builder_tag_vreg().
    ins->ctype = XIR_TYPE_UNKNOWN;
    ins->dst = dst;
    ins->args[0] = a;
    ins->args[1] = b;
    blk->nins++;

    // Link vreg to defining instruction
    if (!xir_ref_is_none(dst)) {
        uint32_t di = XIR_REF_INDEX(dst);
        func->vregs[di].def = ins;
    }

    return dst;
}

XirRef xir_emit_unary(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type, XirRef a) {
    return xir_emit(func, blk, op, type, a, XIR_NONE);
}

void xir_emit_void(XirFunc *func, XirBlock *blk, uint16_t op, XirRef a, XirRef b) {
    XR_DCHECK(func != NULL, "xir_emit_void: NULL func");
    XR_DCHECK(blk != NULL, "xir_emit_void: NULL blk");
    XirIns *ins = block_grow_ins(func, blk);
    if (!ins) return;

    ins->op = op;
    ins->rep = XR_REP_VOID;
    ins->flags = 0;
    if (xir_op_has_side_effect(op))  ins->flags |= XIR_FLAG_SIDE_EFFECT;
    if (xir_op_is_commutative(op))   ins->flags |= XIR_FLAG_COMMUTATIVE;
    ins->ctype = XIR_TYPE_UNKNOWN;
    ins->dst = XIR_NONE;
    ins->args[0] = a;
    ins->args[1] = b;
    blk->nins++;
}

void xir_emit_raw(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type,
                   XirRef dst, XirRef arg0, XirRef arg1) {
    XR_DCHECK(func != NULL, "xir_emit_raw: NULL func");
    XR_DCHECK(blk != NULL, "xir_emit_raw: NULL blk");
    XirIns *ins = block_grow_ins(func, blk);
    if (!ins) return;

    ins->op = op;
    ins->rep = type;
    ins->flags = 0;
    if (xir_op_has_side_effect(op))  ins->flags |= XIR_FLAG_SIDE_EFFECT;
    if (xir_op_is_commutative(op))   ins->flags |= XIR_FLAG_COMMUTATIVE;
    ins->ctype = XIR_TYPE_UNKNOWN;
    ins->dst = dst;
    ins->args[0] = arg0;
    ins->args[1] = arg1;
    blk->nins++;
}

/* ========== Call Arg Pool ========== */

#define INIT_CALL_ARG_POOL 64

uint32_t xir_func_add_call_args(XirFunc *func, const XirRef *args, uint16_t nargs) {
    XR_DCHECK(func != NULL, "xir_func_add_call_args: NULL func");
    if (nargs == 0) return 0;

    // Lazy init
    if (!func->call_arg_pool) {
        uint32_t cap = (nargs > INIT_CALL_ARG_POOL) ? nargs : INIT_CALL_ARG_POOL;
        func->call_arg_pool = (XirRef *)xr_calloc(cap, sizeof(XirRef));
        if (!func->call_arg_pool) return 0;
        func->call_arg_pool_cap = cap;
        func->call_arg_pool_used = 0;
    }

    // Grow if needed
    while (func->call_arg_pool_used + nargs > func->call_arg_pool_cap) {
        uint32_t new_cap = func->call_arg_pool_cap * 2;
        if (!XR_REALLOC(func->call_arg_pool, new_cap * sizeof(XirRef)))
            return 0;
        func->call_arg_pool_cap = new_cap;
    }

    uint32_t start = func->call_arg_pool_used;
    memcpy(&func->call_arg_pool[start], args, nargs * sizeof(XirRef));
    func->call_arg_pool_used += nargs;
    return start;
}

void xir_func_bind_call_args(XirFunc *func, XirRef dst,
                              const XirRef *args, uint16_t nargs) {
    if (!xir_ref_is_vreg(dst) || nargs == 0) return;
    uint32_t vi = XIR_REF_INDEX(dst);
    if (vi >= func->nvreg) return;
    uint32_t start = xir_func_add_call_args(func, args, nargs);
    func->vregs[vi].call_arg_start = start;
    func->vregs[vi].call_nargs = nargs;
}

/* ========== Constants ========== */

// Hash function for constant dedup: mix type byte with raw value bits
static inline uint32_t const_hash(uint8_t type, uint64_t raw) {
    uint64_t h = raw ^ ((uint64_t)type * 0x9E3779B97F4A7C15ULL);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

#define CONST_HT_INIT_SIZE 128  // must be power of 2
#define CONST_HT_LOAD_NUM  3    // grow when nconst * 4 > table_size * 3 (75%)
#define CONST_HT_LOAD_DEN  4

// Rebuild hash table after resize
static void const_ht_rebuild(XirFunc *func) {
    uint32_t size = func->const_ht_mask + 1;
    memset(func->const_ht, 0, size * sizeof(uint32_t));
    for (uint32_t i = 0; i < func->nconst; i++) {
        uint32_t h = const_hash(func->consts[i].rep, func->consts[i].val.raw) & func->const_ht_mask;
        while (func->const_ht[h] != 0)
            h = (h + 1) & func->const_ht_mask;
        func->const_ht[h] = i + 1;  // store index+1 (0 = empty)
    }
}

static XirRef add_const(XirFunc *func, XirConstVal val, uint8_t type) {
    // Initialize hash table on first constant
    if (!func->const_ht) {
        func->const_ht = (uint32_t *)xr_calloc(CONST_HT_INIT_SIZE, sizeof(uint32_t));
        if (!func->const_ht) goto fallback_linear;
        func->const_ht_mask = CONST_HT_INIT_SIZE - 1;
    }

    // Hash-based dedup lookup
    {
        uint32_t h = const_hash(type, val.raw) & func->const_ht_mask;
        while (func->const_ht[h] != 0) {
            uint32_t ci = func->const_ht[h] - 1;
            if (func->consts[ci].rep == type && func->consts[ci].val.raw == val.raw)
                return XIR_REF(XIR_REF_CONST, ci);
            h = (h + 1) & func->const_ht_mask;
        }
    }

    // Not found — insert new constant
    if (func->nconst >= func->const_cap) {
        uint32_t new_cap = func->const_cap * 2;
        if (!XR_REALLOC(func->consts, new_cap * sizeof(XirConst)))
            return XIR_NONE;
        func->const_cap = new_cap;
    }

    uint32_t idx = func->nconst++;
    func->consts[idx].val = val;
    func->consts[idx].rep = type;

    // Insert into hash table; grow if load factor exceeded
    uint32_t ht_size = func->const_ht_mask + 1;
    if (func->nconst * CONST_HT_LOAD_DEN > ht_size * CONST_HT_LOAD_NUM) {
        uint32_t new_size = ht_size * 2;
        uint32_t *new_ht = (uint32_t *)xr_calloc(new_size, sizeof(uint32_t));
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
    return XIR_REF(XIR_REF_CONST, idx);

fallback_linear:
    // Fallback: linear scan if hash table allocation failed
    for (uint32_t i = 0; i < func->nconst; i++) {
        if (func->consts[i].rep == type && func->consts[i].val.raw == val.raw)
            return XIR_REF(XIR_REF_CONST, i);
    }
    if (func->nconst >= func->const_cap) {
        uint32_t new_cap = func->const_cap * 2;
        if (!XR_REALLOC(func->consts, new_cap * sizeof(XirConst)))
            return XIR_NONE;
        func->const_cap = new_cap;
    }
    uint32_t fi = func->nconst++;
    func->consts[fi].val = val;
    func->consts[fi].rep = type;
    return XIR_REF(XIR_REF_CONST, fi);
}

XirRef xir_const_i64(XirFunc *func, int64_t val) {
    XirConstVal cv = { .i64 = val };
    return add_const(func, cv, XR_REP_I64);
}

XirRef xir_const_f64(XirFunc *func, double val) {
    XirConstVal cv = { .f64 = val };
    return add_const(func, cv, XR_REP_F64);
}

XirRef xir_const_ptr(XirFunc *func, void *val) {
    XirConstVal cv = { .ptr = val };
    return add_const(func, cv, XR_REP_PTR);
}

XirRef xir_const_string(XirFunc *func, const char *chars, uint32_t len) {
    XirConstVal cv;
    cv.str.chars = chars;
    cv.str.len = len;
    return add_const(func, cv, XR_REP_STR);
}

/* ========== Phi Nodes ========== */

XirPhi *xir_add_phi(XirFunc *func, XirBlock *blk, uint8_t type) {
    XirPhi *phi = (XirPhi *)xir_arena_calloc(func->arena, 1, sizeof(XirPhi));
    if (!phi) return NULL;

    phi->dst = xir_new_vreg(func, type);
    phi->rep = type;
    switch (type) {
    case XR_REP_I64: phi->ctype = (XirType){ XIR_TK_INT, 0, 0 }; break;
    case XR_REP_F64: phi->ctype = (XirType){ XIR_TK_FLOAT, 0, 0 }; break;
    case XR_REP_PTR: phi->ctype = (XirType){ XIR_TK_PTR, 0, 0 }; break;
    default:         phi->ctype = XIR_TYPE_UNKNOWN; break;
    }
    phi->narg = blk->npred;
    phi->args = (XirRef *)xir_arena_calloc(func->arena, blk->npred, sizeof(XirRef));

    // Initialize all args to XIR_NONE
    for (uint16_t i = 0; i < phi->narg; i++) {
        phi->args[i] = XIR_NONE;
    }

    // Prepend to block's phi list
    phi->next = blk->phis;
    blk->phis = phi;

    return phi;
}

void xir_phi_set_arg(XirPhi *phi, uint32_t pred_idx, XirRef val) {
    XR_DCHECK(pred_idx < phi->narg, "xir_phi_set_arg: pred_idx out of range");
    phi->args[pred_idx] = val;
}

/* ========== Opcode Info ========== */

static const char *op_names[] = {
    [XIR_ADD]       = "add",
    [XIR_SUB]       = "sub",
    [XIR_MUL]       = "mul",
    [XIR_DIV]       = "div",
    [XIR_MOD]       = "mod",
    [XIR_NEG]       = "neg",
    [XIR_AND]       = "and",
    [XIR_OR]        = "or",
    [XIR_XOR]       = "xor",
    [XIR_NOT]       = "not",
    [XIR_SHL]       = "shl",
    [XIR_SHR]       = "shr",
    [XIR_FADD]      = "fadd",
    [XIR_FSUB]      = "fsub",
    [XIR_FMUL]      = "fmul",
    [XIR_FDIV]      = "fdiv",
    [XIR_FNEG]      = "fneg",
    [XIR_I2F]       = "i2f",
    [XIR_F2I]       = "f2i",
    [XIR_EQ]        = "eq",
    [XIR_NE]        = "ne",
    [XIR_LT]        = "lt",
    [XIR_LE]        = "le",
    [XIR_GT]        = "gt",
    [XIR_GE]        = "ge",
    [XIR_FEQ]       = "feq",
    [XIR_FNE]       = "fne",
    [XIR_FLT]       = "flt",
    [XIR_FLE]       = "fle",
    [XIR_BOX_I64]   = "box.i64",
    [XIR_BOX_F64]   = "box.f64",
    [XIR_UNBOX_I64] = "unbox.i64",
    [XIR_UNBOX_F64] = "unbox.f64",
    [XIR_TAG_CHECK] = "tag.check",
    [XIR_TAG_LOAD]  = "tag.load",
    [XIR_LOAD]      = "load",
    [XIR_STORE]     = "store",
    [XIR_LOAD_FIELD]  = "load.field",
    [XIR_STORE_FIELD] = "store.field",
    [XIR_ALLOC]     = "alloc",
    [XIR_STORE_CORO] = "store_coro",
    [XIR_STORE_CORO_BYTE] = "store_coro.b",
    [XIR_LOAD_CORO]  = "load_coro",
    [XIR_LOAD_CORO_BYTE] = "load_coro.b",
    [XIR_LOAD32S]    = "load32s",
    [XIR_LOAD8Z]     = "load8z",
    [XIR_LOAD8S]     = "load8s",
    [XIR_STORE8]     = "store8",
    [XIR_LOAD16Z]    = "load16z",
    [XIR_LOAD16S]    = "load16s",
    [XIR_STORE16]    = "store16",
    [XIR_LOAD32Z]    = "load32z",
    [XIR_STORE32]    = "store32",
    [XIR_LOAD_F32]   = "load.f32",
    [XIR_STORE_F32]  = "store.f32",
    [XIR_GUARD_BOUNDS] = "guard.bounds",
    [XIR_CONST_I64] = "const.i64",
    [XIR_CONST_F64] = "const.f64",
    [XIR_CONST_PTR] = "const.ptr",
    [XIR_JMP]       = "jmp",
    [XIR_BR]        = "br",
    [XIR_RET]       = "ret",
    [XIR_CALL]      = "call",
    [XIR_CALL_C]    = "call.c",
    [XIR_CALL_C_LEAF] = "call.c.leaf",
    [XIR_CALL_SELF_DIRECT] = "call.self.direct",
    [XIR_CALL_KNOWN_REG] = "call.known.reg",
    [XIR_RT_ADD]    = "rt.add",
    [XIR_RT_SUB]    = "rt.sub",
    [XIR_RT_MUL]    = "rt.mul",
    [XIR_RT_DIV]    = "rt.div",
    [XIR_RT_MOD]    = "rt.mod",
    [XIR_RT_UNM]    = "rt.unm",
    [XIR_RT_LT]     = "rt.lt",
    [XIR_RT_LE]     = "rt.le",
    [XIR_RT_EQ]     = "rt.eq",
    [XIR_RT_PRINT]  = "rt.print",
    [XIR_RT_ARRAY_NEW] = "rt.array_new",
    [XIR_RT_ARRAY_PUSH] = "rt.array_push",
    [XIR_RT_ARRAY_LEN] = "rt.array_len",
    [XIR_RT_INDEX_GET] = "rt.index_get",
    [XIR_RT_INDEX_SET] = "rt.index_set",
    [XIR_RT_MAP_NEW] = "rt.map_new",
    [XIR_RT_ISNULL] = "rt.isnull",
    [XIR_SAFEPOINT]    = "safepoint",
    [XIR_BARRIER_FWD]  = "barrier.fwd",
    [XIR_BARRIER_BACK] = "barrier.back",
    [XIR_DEOPT]        = "deopt",
    [XIR_GUARD_TAG]    = "guard.tag",
    [XIR_GUARD_CLASS]  = "guard.class",
    [XIR_GUARD_KLASS]  = "guard.klass",
    [XIR_GUARD_NONNULL] = "guard.nonnull",
    [XIR_GUARD_SHAPE] = "guard.shape",
    [XIR_TRY_BEGIN] = "try.begin",
    [XIR_TRY_END]   = "try.end",
    [XIR_THROW]     = "throw",
    [XIR_CATCH]     = "catch",
    [XIR_RETAIN]    = "arc.retain",
    [XIR_RELEASE]   = "arc.release",
    [XIR_LOAD_UPVAL]  = "load.upval",
    [XIR_STORE_UPVAL] = "store.upval",
    [XIR_MOV]       = "mov",
    [XIR_NOP]       = "nop",
    [XIR_PHI]       = "phi",
    [XIR_REDEFINE]  = "redefine",
};

const char *xir_op_name(uint16_t op) {
    if (op < XIR_OP_COUNT && op_names[op]) {
        return op_names[op];
    }
    if (op >= XIR_MACH_BASE) {
        return "mach.???";
    }
    return "???";
}

bool xir_op_is_commutative(uint16_t op) {
    switch (op) {
        case XIR_ADD: case XIR_MUL:
        case XIR_AND: case XIR_OR: case XIR_XOR:
        case XIR_FADD: case XIR_FMUL:
        case XIR_EQ: case XIR_NE:
        case XIR_FEQ: case XIR_FNE:
        case XIR_RT_ADD: case XIR_RT_MUL:
        case XIR_RT_EQ:
            return true;
        default:
            return false;
    }
}

bool xir_op_is_pure(uint16_t op) {
    // Pure ops: arithmetic, logic, comparison, constants, REDEFINE
    return op <= XIR_FLE || op == XIR_CONST_I64 || op == XIR_CONST_F64
        || op == XIR_REDEFINE;
}

/* ========== Dominator Utilities ========== */

// Compute immediate dominators using Cooper's algorithm.
// Requires blocks[0] = entry. Uses block->id as RPO approximation
// (valid for XIR's structured CFG from builder).
void xir_compute_idom(XirFunc *func, uint32_t *idom, uint32_t nblk) {
    for (uint32_t i = 0; i < nblk; i++) idom[i] = UINT32_MAX;
    idom[0] = 0;

    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = 1; bi < nblk; bi++) {
            XirBlock *blk = func->blocks[bi];
            uint32_t new_idom = UINT32_MAX;
            for (uint32_t p = 0; p < blk->npred; p++) {
                uint32_t pid = blk->preds[p]->id;
                if (pid >= nblk || idom[pid] == UINT32_MAX) continue;
                if (new_idom == UINT32_MAX) {
                    new_idom = pid;
                } else {
                    // intersect
                    uint32_t a = new_idom, b = pid;
                    while (a != b) {
                        while (a > b) a = idom[a];
                        while (b > a) b = idom[b];
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

uint32_t *xir_func_get_idom(XirFunc *func) {
    if (!func || func->nblk == 0) return NULL;
    if (func->idom) return func->idom;

    func->idom = (uint32_t *)xr_malloc(func->nblk * sizeof(uint32_t));
    if (!func->idom) return NULL;
    xir_compute_idom(func, func->idom, func->nblk);

    // Populate block->idom pointers for passes that use them
    for (uint32_t i = 0; i < func->nblk; i++) {
        if (func->idom[i] != UINT32_MAX && func->idom[i] < func->nblk)
            func->blocks[i]->idom = func->blocks[func->idom[i]];
    }
    return func->idom;
}

void xir_func_invalidate_idom(XirFunc *func) {
    if (!func) return;
    if (func->idom) {
        xr_free(func->idom);
        func->idom = NULL;
    }
}

void xir_rebuild_vreg_defs(XirFunc *func) {
    if (!func) return;
    // Clear all def pointers first
    for (uint32_t v = 0; v < func->nvreg; v++)
        func->vregs[v].def = NULL;
    // Rescan all instructions and set def pointers
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t v = XIR_REF_INDEX(ins->dst);
                if (v < func->nvreg)
                    func->vregs[v].def = ins;
            }
        }
    }
}

bool xir_dominates(uint32_t *idom, uint32_t a, uint32_t b) {
    // Walk the idom chain from b up to entry.  The entry node is its own
    // immediate dominator (idom[0] == 0) which serves as the sentinel — we
    // stop as soon as b reaches 0.  A non-entry node pointing to itself would
    // indicate corrupt idom data, so we defend against it.
    while (b != a) {
        if (b == 0) return (a == 0);
        uint32_t next = idom[b];
        if (next == UINT32_MAX || next == b) return false;
        b = next;
    }
    return true;
}

bool xir_op_has_side_effect(uint16_t op) {
    switch (op) {
        // Memory stores
        case XIR_STORE: case XIR_STORE_FIELD:
        case XIR_STORE8: case XIR_STORE16: case XIR_STORE32: case XIR_STORE_F32:
        case XIR_STORE_CORO: case XIR_STORE_CORO_BYTE:
        case XIR_STORE_UPVAL:
        // Calls
        case XIR_CALL: case XIR_CALL_C: case XIR_CALL_C_LEAF:
        case XIR_CALL_SELF_DIRECT: case XIR_CALL_DIRECT:
        case XIR_CALL_KNOWN: case XIR_CALL_KNOWN_REG:
        // Mixed-type runtime helpers (may allocate, throw, or have observable effects)
        case XIR_RT_ADD: case XIR_RT_SUB: case XIR_RT_MUL:
        case XIR_RT_DIV: case XIR_RT_MOD: case XIR_RT_UNM:
        case XIR_RT_LT: case XIR_RT_LE: case XIR_RT_EQ:
        case XIR_RT_PRINT:
        case XIR_RT_ARRAY_NEW: case XIR_RT_ARRAY_PUSH: case XIR_RT_ARRAY_LEN:
        case XIR_RT_INDEX_GET: case XIR_RT_INDEX_SET:
        case XIR_RT_MAP_NEW: case XIR_RT_ISNULL:
        // GC / barriers
        case XIR_SAFEPOINT:
        case XIR_BARRIER_FWD: case XIR_BARRIER_BACK:
        case XIR_RETAIN: case XIR_RELEASE:
        case XIR_ALLOC:
        // Guards (may deopt — observable side effect)
        case XIR_DEOPT:
        case XIR_GUARD_TAG: case XIR_GUARD_CLASS: case XIR_GUARD_KLASS:
        case XIR_GUARD_NONNULL:
        case XIR_GUARD_SHAPE: case XIR_GUARD_BOUNDS:
        // Exception handling
        case XIR_THROW:
        case XIR_TRY_BEGIN: case XIR_TRY_END:
        case XIR_CATCH:
            return true;
        default:
            return false;
    }
}
