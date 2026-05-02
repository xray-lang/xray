/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit.c - Xi IR to VM bytecode emitter (driver)
 *
 * Translates typed SSA IR (XiFunc) into register-based bytecode
 * targeting the existing Xray VM (XrProto / xchunk.h format).
 *
 * Instruction selection is table-driven: each XiOp maps to a handler
 * function defined in a domain-specific sub-file (xi_emit_arith.c, etc.).
 */

#include "xi_emit_internal.h"
#include "xi_analysis.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/xexec_state.h"

/* ========== Helpers ========== */

XR_FUNC void emit_error(EmitCtx *ctx, XiEmitStatus s) {
    if (ctx->status == XI_EMIT_OK)
        ctx->status = s;
}

XR_FUNC int current_pc(EmitCtx *ctx) {
    return PROTO_CODE_COUNT(ctx->proto);
}

XR_FUNC void emit_inst(EmitCtx *ctx, XrInstruction inst) {
    xr_vm_proto_write(ctx->proto, inst, ctx->current_line);
}

/* Return a register to the free pool for reuse. */
XR_FUNC void free_reg(EmitCtx *ctx, uint8_t reg) {
    if (reg == NO_REG) return;
    if (ctx->nfree < MAX_REGS) {
        ctx->free_regs[ctx->nfree++] = reg;
    }
}

/* Get register for a value. Assigns one if not yet mapped.
 * Uses free register stack before allocating new ones. */
XR_FUNC uint8_t reg_of(EmitCtx *ctx, const XiValue *v) {
    XR_DCHECK(v != NULL, "reg_of: NULL value");

    if (v->id >= ctx->reg_map_size) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return 0;
    }

    if (ctx->reg_map[v->id] == NO_REG) {
        /* Try recycled register first */
        if (ctx->nfree > 0) {
            ctx->reg_map[v->id] = ctx->free_regs[--ctx->nfree];
        } else {
            if (ctx->next_reg >= MAX_REGS - 1) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
                return 0;
            }
            ctx->reg_map[v->id] = ctx->next_reg++;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
        }
    }
    return ctx->reg_map[v->id];
}

/* Like reg_of but never uses the free list — always allocates from next_reg.
 * Call instructions place args at dst+1..dst+nargs; a recycled low register
 * for dst could overlap with live source registers and cause clobber bugs. */
XR_FUNC uint8_t alloc_reg_fresh(EmitCtx *ctx, const XiValue *v) {
    XR_DCHECK(v != NULL, "alloc_reg_fresh: NULL value");
    if (v->id >= ctx->reg_map_size) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return 0;
    }
    if (ctx->reg_map[v->id] == NO_REG) {
        if (ctx->next_reg >= MAX_REGS - 1) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return 0;
        }
        ctx->reg_map[v->id] = ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
    }
    return ctx->reg_map[v->id];
}

/* Release registers of input args whose last use is at the current ordinal.
 * Called AFTER emitting an instruction that reads these args. */
XR_FUNC void try_free_args(EmitCtx *ctx, const XiValue *v) {
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg->id >= ctx->reg_map_size) continue;
        /* Free register if this is the last use of arg */
        if (ctx->last_use[arg->id] == ctx->current_ordinal) {
            uint8_t r = ctx->reg_map[arg->id];
            ctx->reg_map[arg->id] = NO_REG;
            free_reg(ctx, r);
        }
    }
}

/* Add a pending jump patch. */
static void add_patch(EmitCtx *ctx, int pc, uint32_t target_bid) {
    if (ctx->npatch >= ctx->patch_cap) {
        uint32_t new_cap = ctx->patch_cap ? ctx->patch_cap * 2 : 16;
        void *tmp = xr_realloc(ctx->patches, new_cap * sizeof(ctx->patches[0]));
        if (!tmp) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        ctx->patches = tmp;
        ctx->patch_cap = new_cap;
    }
    ctx->patches[ctx->npatch].pc = pc;
    ctx->patches[ctx->npatch].target_bid = target_bid;
    ctx->npatch++;
}

/* Add a pending OP_TRY patch (catch + finally absolute PC targets). */
XR_FUNC void add_try_patch(EmitCtx *ctx, int pc, uint32_t catch_bid,
                           uint32_t finally_bid) {
    if (ctx->ntry_patch >= ctx->try_patch_cap) {
        uint32_t new_cap = ctx->try_patch_cap ? ctx->try_patch_cap * 2 : 4;
        void *tmp = xr_realloc(ctx->try_patches, new_cap * sizeof(ctx->try_patches[0]));
        if (!tmp) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        ctx->try_patches = tmp;
        ctx->try_patch_cap = new_cap;
    }
    ctx->try_patches[ctx->ntry_patch].pc = pc;
    ctx->try_patches[ctx->ntry_patch].target_bid = catch_bid;
    ctx->try_patches[ctx->ntry_patch].finally_bid = finally_bid;
    ctx->ntry_patch++;
}

/* Add constant to pool, return index. */
XR_FUNC int add_const_int(EmitCtx *ctx, int64_t val) {
    XrValue xv = xr_make_int_val(val, XR_TAG_I64);
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

XR_FUNC int add_const_float(EmitCtx *ctx, double val) {
    XrValue xv = xr_make_float_val(val, XR_TAG_F64);
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

XR_FUNC int add_const_string(EmitCtx *ctx, const char *str) {
    XrValue xv;
    if (ctx->isolate && str) {
        XrString *xs = xr_compile_time_intern(ctx->isolate, str, strlen(str));
        xv = xr_string_value(xs);
    } else {
        /* No isolate: raw C string pointers are not valid GC objects,
         * so xr_make_ptr_val would read garbage GC header bytes.
         * Use null placeholder — callers without isolate only check
         * instruction sequences, not constant pool values. */
        xv = xr_null();
    }
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

/* Add a method name to the proto's local symbol table.  Returns the local
 * symbol index suitable for OP_INVOKE's B field.  Requires an isolate
 * (for the global symbol table); returns -1 on error. */
XR_FUNC int add_symbol(EmitCtx *ctx, const char *name) {
    if (!ctx->isolate || !name) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return -1;
    }
    XrSymbolTable *st = (XrSymbolTable *)xr_isolate_get_symbol_table(ctx->isolate);
    XR_DCHECK(st != NULL, "isolate must have a symbol table");
    SymbolId global = xr_symbol_register_in_table(st, name);
    int local = xr_proto_add_symbol(ctx->proto, (int32_t)global);
    if (local > MAXARG_B) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
        return -1;
    }
    return local;
}

/* ========== Last-Use Computation ========== */

/* Pre-compute last-use ordinals for register recycling.
 * Walks all blocks in RPO, assigning each value a monotonic ordinal.
 * For each arg reference, updates last_use[arg_id] = max ordinal.
 * Also accounts for block terminators that reference values. */
static void compute_last_use(EmitCtx *ctx) {
    uint32_t ord = 1;
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            /* Record ordinal for this value */
            /* Update last-use of all args referenced by this value */
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *arg = v->args[a];
                if (arg && arg->id < ctx->reg_map_size)
                    ctx->last_use[arg->id] = ord;
            }
            ord++;
        }

        /* Terminator references: control value and phi args in successors */
        if (blk->control && blk->control->id < ctx->reg_map_size)
            ctx->last_use[blk->control->id] = ord;

        /* Phi args from this block's successors reference values too */
        for (int s = 0; s < 2; s++) {
            XiBlock *succ = blk->succs[s];
            if (!succ) continue;
            int pred_idx = -1;
            for (uint16_t p = 0; p < succ->npreds; p++) {
                if (succ->preds[p] == blk) { pred_idx = (int)p; break; }
            }
            if (pred_idx < 0) continue;
            for (XiPhi *phi = succ->phis; phi; phi = phi->next) {
                if ((uint16_t)pred_idx < phi->value.nargs) {
                    XiValue *src = phi->value.args[pred_idx];
                    if (src && src->id < ctx->reg_map_size)
                        ctx->last_use[src->id] = ord;
                }
            }
        }
        ord++;  /* account for terminator */
    }

    /* Phi registers must never be freed: they are referenced by
     * emit_phi_moves from any predecessor, which is not captured
     * by the ordinal-based last-use tracking above. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk) continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (phi->value.id < ctx->reg_map_size)
                ctx->last_use[phi->value.id] = UINT32_MAX;
        }
    }

    /* Loop-invariant liveness: values defined outside a loop but used inside
     * must stay live for the entire loop — the single RPO walk above only
     * records one ordinal per use, but the VM re-executes loop blocks.
     *
     * Algorithm: detect back edges (succ.rpo <= block.rpo).  For each back
     * edge target (loop header), any value whose def-block RPO < header RPO
     * and whose last_use falls inside the loop range must be pinned. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk) continue;
        for (int s = 0; s < 2; s++) {
            XiBlock *succ = blk->succs[s];
            if (!succ || succ->rpo == 0) continue;
            if (succ->rpo > blk->rpo) continue;   /* not a back edge */

            /* Back edge: blk → succ.  Loop spans RPO [succ->rpo, blk->rpo].
             * Pin every value defined before the loop that is used inside. */
            uint32_t loop_lo = succ->rpo;
            uint32_t loop_hi = blk->rpo;
            for (uint32_t lr = loop_lo; lr <= loop_hi; lr++) {
                XiBlock *lb = ctx->rpo_order[lr];
                if (!lb) continue;
                for (uint32_t i = 0; i < lb->nvalues; i++) {
                    XiValue *v = lb->values[i];
                    for (uint16_t a = 0; a < v->nargs; a++) {
                        XiValue *arg = v->args[a];
                        if (!arg || arg->id >= ctx->reg_map_size) continue;
                        if (!arg->block) continue;
                        if (arg->block->rpo > 0 && arg->block->rpo < loop_lo)
                            ctx->last_use[arg->id] = UINT32_MAX;
                    }
                }
            }
        }
    }
}

/* ========== Register Allocation ========== */

/* Params get R[0..nparams-1], phis pre-assigned, last-use computed. */
static void alloc_registers(EmitCtx *ctx) {
    XiFunc *f = ctx->func;

    /* Assign parameter registers by scanning entry block for XI_PARAM ops.
     * This is robust whether f->params is populated or not. */
    XiBlock *entry = f->entry;
    if (entry) {
        for (uint32_t i = 0; i < entry->nvalues; i++) {
            XiValue *v = entry->values[i];
            if (v->op == XI_PARAM) {
                uint16_t pidx = (uint16_t)v->aux_int;
                if (v->id < ctx->reg_map_size && pidx < MAX_REGS) {
                    ctx->reg_map[v->id] = (uint8_t)pidx;
                    if (pidx + 1 > ctx->next_reg) {
                        ctx->next_reg = (uint8_t)(pidx + 1);
                        ctx->max_reg = ctx->next_reg;
                    }
                }
            }
        }
    }

    /* Pre-assign phi registers to avoid conflicts with phi moves.
     * Phis get their own registers before instruction values. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            (void)reg_of(ctx, &phi->value);
            if (ctx->status != XI_EMIT_OK) return;
        }
    }
}

/* ========== Phi Elimination ========== */

/* Emit MOVE instructions for phi nodes when transitioning from
 * predecessor 'pred' to successor 'succ'. */
static void emit_phi_moves(EmitCtx *ctx, XiBlock *pred, XiBlock *succ) {
    /* Find which predecessor index 'pred' is in succ->preds */
    int pred_idx = -1;
    for (uint16_t p = 0; p < succ->npreds; p++) {
        if (succ->preds[p] == pred) { pred_idx = (int)p; break; }
    }
    if (pred_idx < 0) return;

    for (XiPhi *phi = succ->phis; phi; phi = phi->next) {
        if ((uint16_t)pred_idx >= phi->value.nargs) continue;
        XiValue *src = phi->value.args[pred_idx];
        if (!src) continue;

        uint8_t dst_reg = reg_of(ctx, &phi->value);
        uint8_t src_reg = reg_of(ctx, src);
        if (ctx->status != XI_EMIT_OK) return;

        if (dst_reg != src_reg) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst_reg, src_reg, 0));
        }
    }
}

/* Class helpers, field/method proto emission, and AST-to-value conversion
 * are in xi_emit_object.c (emit_class_create_impl). */

/* ========== Local Handlers (CONST, PARAM, COPY, SELECT) ========== */

static void emit_const(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    struct XrType *ty = v->type;
    if (!ty) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

    switch (ty->kind) {
        case XR_KIND_INT: {
            int64_t val = v->aux_int;
            if (val >= LOADI_MIN && val <= LOADI_MAX) {
                emit_inst(ctx, CREATE_AsBx(OP_LOADI, dst, (int)val));
            } else {
                int ki = add_const_int(ctx, val);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            }
            break;
        }
        case XR_KIND_FLOAT: {
            double fval;
            memcpy(&fval, &v->aux_int, sizeof(double));
            int sv = (int)fval;
            if ((double)sv == fval && sv >= LOADI_MIN && sv <= LOADI_MAX) {
                emit_inst(ctx, CREATE_AsBx(OP_LOADF, dst, sv));
            } else {
                int ki = add_const_float(ctx, fval);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            }
            break;
        }
        case XR_KIND_BOOL:
            if (v->aux_int)
                emit_inst(ctx, CREATE_ABC(OP_LOADTRUE, dst, 0, 0));
            else
                emit_inst(ctx, CREATE_ABC(OP_LOADFALSE, dst, 0, 0));
            break;
        case XR_KIND_NULL:
            emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
            break;
        case XR_KIND_STRING: {
            const char *s = (const char *)v->aux;
            int ki = add_const_string(ctx, s);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            break;
        }
        default: {
            /* Generic pointer constant (enum type, etc.) */
            void *ptr = v->aux;
            if (ptr) {
                XrValue xv = XR_FROM_PTR(ptr);
                int ki = xr_vm_proto_add_constant(ctx->proto, xv);
                if (ki > MAXARG_Bx) {
                    emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
                    return;
                }
                emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            } else {
                emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
            }
            break;
        }
    }
}

static void emit_param(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)ctx; (void)v; (void)dst;
    /* Params already in registers; no-op. */
}

static void emit_copy(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    if (dst != src)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
}

/* Conditional select: dst = cond ? true_val : false_val.
 * Emitted as: MOVE dst, false_val; TEST cond, 0; MOVE dst, true_val.
 * TEST skips the next instruction when cond is falsy. */
static void emit_select(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 3) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t cond_r = reg_of(ctx, v->args[0]);
    uint8_t true_r = reg_of(ctx, v->args[1]);
    uint8_t false_r = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK) return;
    if (dst != false_r)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, false_r, 0));
    emit_inst(ctx, CREATE_ABC(OP_TEST, cond_r, 0, 0));
    if (dst != true_r)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, true_r, 0));
}

/* ========== Dispatch Table ========== */

const XiEmitHandler xi_emit_handlers[XI_OP_COUNT] = {
    [XI_CONST]        = emit_const,
    [XI_PARAM]        = emit_param,
    [XI_ADD]          = xi_emit_arith,
    [XI_SUB]          = xi_emit_arith,
    [XI_MUL]          = xi_emit_arith,
    [XI_DIV]          = xi_emit_arith,
    [XI_MOD]          = xi_emit_arith,
    [XI_NEG]          = xi_emit_neg,
    [XI_BAND]         = xi_emit_arith,
    [XI_BOR]          = xi_emit_arith,
    [XI_BXOR]         = xi_emit_arith,
    [XI_BNOT]         = xi_emit_bnot,
    [XI_SHL]          = xi_emit_arith,
    [XI_SHR]          = xi_emit_arith,
    [XI_EQ]           = xi_emit_cmp,
    [XI_NE]           = xi_emit_cmp,
    [XI_LT]           = xi_emit_cmp,
    [XI_LE]           = xi_emit_cmp,
    [XI_GT]           = xi_emit_cmp,
    [XI_GE]           = xi_emit_cmp,
    [XI_NOT]          = xi_emit_not,
    [XI_CONVERT]      = xi_emit_convert,
    [XI_BOX]          = xi_emit_box,
    [XI_UNBOX]        = xi_emit_unbox,
    [XI_LOAD_FIELD]   = xi_emit_load_field,
    [XI_STORE_FIELD]  = xi_emit_store_field,
    [XI_INDEX_GET]    = xi_emit_index_get,
    [XI_INDEX_SET]    = xi_emit_index_set,
    [XI_ALLOC]        = xi_emit_alloc,
    [XI_ARRAY_NEW]    = xi_emit_array_new,
    [XI_MAP_NEW]      = xi_emit_map_new,
    [XI_CALL]         = xi_emit_call,
    [XI_CALL_METHOD]  = xi_emit_call_method,
    [XI_CALL_BUILTIN] = xi_emit_call_builtin,
    [XI_EXTRACT]      = xi_emit_extract,
    [XI_CLOSURE_NEW]  = xi_emit_closure_new,
    [XI_LOAD_UPVAL]   = xi_emit_load_upval,
    [XI_STORE_UPVAL]  = xi_emit_store_upval,
    [XI_GET_SHARED]   = xi_emit_get_shared,
    [XI_SET_SHARED]   = xi_emit_set_shared,
    [XI_PRINT]        = xi_emit_print,
    [XI_GO]           = xi_emit_go,
    [XI_AWAIT]        = xi_emit_await,
    [XI_CHAN_SEND]     = xi_emit_chan_send,
    [XI_CHAN_RECV]     = xi_emit_chan_recv,
    [XI_YIELD]        = xi_emit_yield,
    [XI_THROW]        = xi_emit_throw,
    [XI_ITER_NEW]     = xi_emit_iter,
    [XI_ITER_NEXT]    = xi_emit_iter,
    [XI_ITER_VALID]   = xi_emit_iter,
    [XI_DEFER]        = xi_emit_defer,
    [XI_CHAN_NEW]      = xi_emit_chan_new,
    [XI_SET_NEW]      = xi_emit_set_new,
    [XI_STR_CONCAT]   = xi_emit_str_concat,
    [XI_IS]           = xi_emit_is,
    [XI_AS]           = xi_emit_as,
    [XI_SLICE]        = xi_emit_slice,
    [XI_RANGE]        = xi_emit_range,
    [XI_MULTI_RET]    = xi_emit_multi_ret,
    [XI_ISNULL]       = xi_emit_isnull,
    [XI_PHI]          = NULL,  /* handled separately by emit_phi_moves */
    [XI_SELECT]       = emit_select,
    [XI_COPY]         = emit_copy,
    [XI_CLASS_CREATE] = xi_emit_class_create,
    [XI_SCOPE_ENTER]  = xi_emit_scope_enter,
    [XI_SCOPE_EXIT]   = xi_emit_scope_exit,
    [XI_TRY]          = xi_emit_try,
    [XI_CATCH]        = xi_emit_catch,
    [XI_FINALLY]      = xi_emit_finally,
    [XI_END_TRY]      = xi_emit_end_try,
    [XI_ASSERT]       = xi_emit_assert,
    [XI_ASSERT_EQ]    = xi_emit_assert_eq,
    [XI_ASSERT_NE]    = xi_emit_assert_ne,
    [XI_TYPEOF]       = xi_emit_typeof,
    [XI_GET_BUILTIN]  = xi_emit_get_builtin,
    [XI_IMPORT_REF]   = xi_emit_import_ref,
};

/* ========== Instruction Selection ========== */

static void emit_value(EmitCtx *ctx, XiValue *v) {
    if (ctx->status != XI_EMIT_OK) return;

    /* Skip comparison that was absorbed into the block terminator */
    if (v == ctx->fused_cmp) return;

    /* Call-like ops place args at dst+1..dst+nargs.  A recycled low register
     * for dst could overlap with live source arg registers (clobber bug).
     * Force fresh allocation so dst > all previously allocated registers. */
    bool call_like = (v->op == XI_CALL || v->op == XI_CALL_METHOD
                      || v->op == XI_GO || v->op == XI_MULTI_RET);
    uint8_t dst = call_like ? alloc_reg_fresh(ctx, v) : reg_of(ctx, v);
    if (ctx->status != XI_EMIT_OK) return;

    XR_DCHECK(v->op >= 0 && v->op < XI_OP_COUNT, "emit_value: op out of range");
    XiEmitHandler handler = xi_emit_handlers[v->op];
    if (handler) {
        handler(ctx, v, dst);
    } else {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
    }
}

/* ========== Block Emission ========== */

/* Check if a comparison value is only used as the block control (no other
 * consumers).  If so, we can fuse it into the branch-form opcode. */
static bool can_fuse_cmp(XiBlock *blk, XiValue *ctrl) {
    XR_DCHECK(ctrl != NULL, "ctrl must not be NULL");
    uint16_t op = ctrl->op;
    if (op < XI_EQ || op > XI_GE || ctrl->nargs < 2) return false;
    /* Ensure no other value in this block uses the comparison result */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (v == ctrl) continue;
        for (uint16_t a = 0; a < v->nargs; a++) {
            if (v->args[a] == ctrl) return false;
        }
    }
    return true;
}

/* Emit OP_EXPORT instructions for all named exports in the module-level
 * function.  Called just before the return terminator so that the VM's
 * module system can record the exported values.  Uses a scratch register
 * (next_reg) to load each shared variable before exporting it.
 *
 * Format:  OP_EXPORT A=const_idx(name), B=reg(value), C=0 */
static void emit_module_exports(EmitCtx *ctx) {
    XiFunc *f = ctx->func;
    if (!f->export_names || f->nshared == 0 || !ctx->isolate)
        return;

    uint8_t tmp = ctx->next_reg;
    if ((int)(tmp + 1) > ctx->max_reg)
        ctx->max_reg = (uint8_t)(tmp + 1);

    for (uint16_t i = 0; i < f->nshared; i++) {
        const char *name = f->export_names[i];
        if (!name) continue;
        int name_idx = add_const_string(ctx, name);
        if (ctx->status != XI_EMIT_OK) return;
        /* OP_GETSHARED tmp, G[shared_offset + i] */
        emit_inst(ctx, CREATE_ABx(OP_GETSHARED, tmp, (int)i));
        /* OP_EXPORT K[name_idx], tmp, 0 */
        emit_inst(ctx, CREATE_ABC(OP_EXPORT, (uint8_t)name_idx, tmp, 0));
    }
}

static void emit_block(EmitCtx *ctx, XiBlock *blk, XiBlock *next_blk) {
    if (ctx->status != XI_EMIT_OK) return;

    /* Record block start PC */
    XR_DCHECK(blk->id < ctx->block_pc_size, "block_id out of range");
    ctx->block_pc[blk->id] = current_pc(ctx);

    /* Detect fuseable comparison for IF blocks */
    ctx->fused_cmp = NULL;
    if (blk->kind == XI_BLOCK_IF && blk->control)
        if (can_fuse_cmp(blk, blk->control))
            ctx->fused_cmp = blk->control;

    /* Emit instruction values with register recycling */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        ctx->current_ordinal++;
        ctx->current_line = (int)blk->values[i]->line;
        emit_value(ctx, blk->values[i]);
        if (ctx->status != XI_EMIT_OK) return;
        /* Recycle registers of args whose last use was this instruction.
         * Skip recycling for the fused comparison's args — they are still
         * needed by the branch-form opcode in the terminator. */
        if (ctx->last_use && blk->values[i] != ctx->fused_cmp)
            try_free_args(ctx, blk->values[i]);
    }

    ctx->current_ordinal++;  /* ordinal for terminator */
    /* Emit terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN:
            /* Emit OP_EXPORT instructions for module-level exports
             * before returning, so the VM's module system picks them up. */
            emit_module_exports(ctx);
            if (ctx->status != XI_EMIT_OK) return;

            if (blk->control && blk->control->op == XI_MULTI_RET) {
                /* Multi-value return: values in consecutive regs */
                uint8_t base = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK) return;
                uint8_t nret = (uint8_t)blk->control->nargs;
                emit_inst(ctx, CREATE_ABC(OP_RETURN, base, nret, 0));
            } else if (blk->control) {
                uint8_t r = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK) return;
                /* If the return value was cell-wrapped for closure capture,
                 * dereference the cell to return the actual value. */
                if (blk->control->id < ctx->reg_map_size &&
                    ctx->cell_wrapped[blk->control->id]) {
                    emit_inst(ctx, CREATE_ABC(OP_CELL_GET, r, r, 0));
                }
                emit_inst(ctx, CREATE_ABC(OP_RETURN1, r, 0, 0));
            } else {
                emit_inst(ctx, CREATE_ABC(OP_RETURN0, 0, 0, 0));
            }
            break;

        case XI_BLOCK_PLAIN: {
            XiBlock *succ = blk->succs[0];
            if (!succ) break;
            /* Emit phi moves for successor */
            emit_phi_moves(ctx, blk, succ);
            if (ctx->status != XI_EMIT_OK) return;
            /* Jump to successor (skip if it's the next block in RPO) */
            if (succ != next_blk) {
                int jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0));  /* placeholder */
                add_patch(ctx, jmp_pc, succ->id);
            }
            break;
        }

        case XI_BLOCK_IF: {
            if (!blk->control) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

            XiBlock *then_b = blk->succs[0];
            XiBlock *else_b = blk->succs[1];
            XR_DCHECK(then_b && else_b, "IF block missing successor");

            if (ctx->fused_cmp) {
                /* Fused comparison-branch: emit OP_LT/LE/EQ etc. directly.
                 * Saves one instruction vs OP_CMP_* + OP_TEST. */
                XiValue *cmp = ctx->fused_cmp;
                XiValue *lhs = cmp->args[0];
                XiValue *rhs = cmp->args[1];
                uint8_t a = reg_of(ctx, lhs);
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK) return;

                /* Determine branch-form opcode and sense.  GT/GE swap args. */
                OpCode branch_op;
                int k = 0;
                bool swap = false;
                switch (cmp->op) {
                    case XI_LT: branch_op = OP_LT; break;
                    case XI_LE: branch_op = OP_LE; break;
                    case XI_GT: branch_op = OP_LT; swap = true; break;
                    case XI_GE: branch_op = OP_LE; swap = true; break;
                    case XI_EQ: branch_op = OP_EQ; break;
                    case XI_NE: branch_op = OP_EQ; k = 1; break;
                    default:    branch_op = OP_EQ; break;
                }
                if (swap) { uint8_t t = a; a = b; b = t; }

                /* Try immediate form (OP_LTI/LEI/EQI) when RHS is small int */
                bool is_imm = false;
                XiValue *imm_arg = swap ? lhs : rhs;   /* the "B" operand */
                XiValue *reg_arg = swap ? rhs : lhs;    /* the "A" operand */
                if (imm_arg->op == XI_CONST && imm_arg->type &&
                    imm_arg->type->kind == XR_KIND_INT &&
                    imm_arg->aux_int >= -128 && imm_arg->aux_int <= 127 &&
                    (branch_op == OP_LT || branch_op == OP_LE || branch_op == OP_EQ)) {
                    OpCode imm_op = branch_op == OP_LT ? OP_LTI :
                                   branch_op == OP_LE ? OP_LEI : OP_EQI;
                    uint8_t ra = reg_of(ctx, reg_arg);
                    if (ctx->status != XI_EMIT_OK) return;
                    int8_t imm = (int8_t)imm_arg->aux_int;
                    emit_inst(ctx, CREATE_ABC(imm_op, ra, (uint8_t)imm, (uint8_t)k));
                    is_imm = true;
                }

                if (!is_imm) {
                    emit_inst(ctx, CREATE_ABC(branch_op, a, b, (uint8_t)k));
                }
            } else {
                /* Non-fused path: TEST cond, skip next if cond is false */
                uint8_t cond = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_TEST, cond, 0, 0));
            }

            /* JMP -> else block */
            int else_jmp_pc = current_pc(ctx);
            emit_inst(ctx, CREATE_sJ(OP_JMP, 0));  /* placeholder */
            add_patch(ctx, else_jmp_pc, else_b->id);

            /* Phi moves for then path */
            emit_phi_moves(ctx, blk, then_b);
            if (ctx->status != XI_EMIT_OK) return;

            /* Jump to then if not fallthrough */
            if (then_b != next_blk) {
                int then_jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
                add_patch(ctx, then_jmp_pc, then_b->id);
            }
            break;
        }

        case XI_BLOCK_UNREACHABLE:
            /* Emit a NOP as placeholder */
            emit_inst(ctx, CREATE_ABC(OP_NOP, 0, 0, 0));
            break;

        default:
            break;
    }
}

/* ========== Jump Patching ========== */

static void patch_jumps(EmitCtx *ctx) {
    for (uint32_t i = 0; i < ctx->npatch; i++) {
        int pc = ctx->patches[i].pc;
        uint32_t bid = ctx->patches[i].target_bid;

        XR_DCHECK(bid < ctx->block_pc_size, "patch: bad block id");
        int target_pc = ctx->block_pc[bid];

        if (target_pc < 0) {
            /* Unreachable target — should not happen in valid IR.
             * Point to current PC as fallback. */
            target_pc = pc + 1;
        }

        /* sJ = target_pc - (pc + 1) */
        int offset = target_pc - (pc + 1);
        XrInstruction *inst = PROTO_CODE_PTR(ctx->proto, pc);
        *inst = CREATE_sJ(OP_JMP, offset);
    }

    /* Patch OP_TRY instructions: Bx = absolute catch PC;
     * Patch the following NOP with finally PC (Bx). */
    for (uint32_t i = 0; i < ctx->ntry_patch; i++) {
        int pc = ctx->try_patches[i].pc;
        uint32_t bid = ctx->try_patches[i].target_bid;
        uint32_t fbid = ctx->try_patches[i].finally_bid;
        XR_DCHECK(bid < ctx->block_pc_size, "try_patch: bad catch block id");
        int target_pc = ctx->block_pc[bid];
        if (target_pc < 0) target_pc = pc + 1;
        XrInstruction *inst = PROTO_CODE_PTR(ctx->proto, pc);
        *inst = CREATE_ABx(OP_TRY, 0, target_pc);

        /* Patch NOP at pc+1 with finally absolute PC */
        if (fbid > 0 && fbid < ctx->block_pc_size) {
            int fin_pc = ctx->block_pc[fbid];
            if (fin_pc >= 0) {
                XrInstruction *fin_inst = PROTO_CODE_PTR(ctx->proto, pc + 1);
                *fin_inst = CREATE_ABx(OP_NOP, 0, fin_pc);
            }
        }
    }
}

/* ========== Slot Map Generation ========== */

/* Build an XiSlotMap from the emitter's register assignment.
 * Scans all Xi IR values and records their bytecode register mappings.
 * Returns NULL on allocation failure (non-fatal). */
static XiSlotMap *build_slot_map(EmitCtx *ctx) {
    XiFunc *f = ctx->func;
    XR_DCHECK(f != NULL, "build_slot_map: NULL func");

    /* Count mapped values */
    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->reg_map_size; i++) {
        if (ctx->reg_map[i] != NO_REG)
            count++;
    }
    if (count == 0) return NULL;

    XiSlotMap *map = (XiSlotMap *)xr_calloc(1, sizeof(XiSlotMap));
    if (!map) return NULL;

    map->entries = (XiSlotMapEntry *)xr_malloc(count * sizeof(XiSlotMapEntry));
    if (!map->entries) {
        xr_free(map);
        return NULL;
    }
    map->capacity = count;

    /* Fill entries from all blocks' values */
    uint32_t idx = 0;
    for (uint32_t b = 0; b < f->nblocks && idx < count; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t vi = 0; vi < blk->nvalues && idx < count; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->id >= ctx->reg_map_size) continue;
            uint8_t reg = ctx->reg_map[v->id];
            if (reg == NO_REG) continue;

            map->entries[idx].value_id = v->id;
            map->entries[idx].bc_slot = reg;
            /* Derive XR_TAG from Xi IR type */
            uint8_t tag = 5; /* XR_TAG_PTR default */
            if (v->type) {
                switch (v->type->kind) {
                    case XR_KIND_INT:   tag = 3; break; /* XR_TAG_I64 */
                    case XR_KIND_FLOAT: tag = 4; break; /* XR_TAG_F64 */
                    case XR_KIND_BOOL:  tag = 1; break; /* XR_TAG_BOOL */
                    case XR_KIND_NULL:
                    case XR_KIND_VOID:  tag = 0; break; /* XR_TAG_NULL */
                    default:            tag = 5; break; /* XR_TAG_PTR */
                }
            }
            map->entries[idx].xr_tag = tag;
            /* bc_pc: prefer per-value IC instruction offset when available
             * (recorded for GETPROP/SETPROP/INVOKE), fallback to block start */
            if (ctx->value_pc && v->id < ctx->reg_map_size &&
                ctx->value_pc[v->id] >= 0) {
                map->entries[idx].bc_pc = (uint32_t)ctx->value_pc[v->id];
            } else {
                map->entries[idx].bc_pc = (blk->id < ctx->block_pc_size &&
                                           ctx->block_pc[blk->id] >= 0)
                                              ? (uint32_t)ctx->block_pc[blk->id]
                                              : 0;
            }
            idx++;
        }
    }
    map->count = idx;
    return map;
}

/* ========== Public API ========== */

XR_FUNC XiEmitStatus xi_emit(XiFunc *f, struct XrayIsolate *isolate,
                              struct XrProto **out_proto) {
    XR_DCHECK(f != NULL, "xi_emit: NULL func");
    XR_DCHECK(out_proto != NULL, "xi_emit: NULL out_proto");
    *out_proto = NULL;

    /* Run prerequisite analyses */
    uint32_t rpo_count = xi_compute_rpo(f);
    if (rpo_count == 0) return XI_EMIT_ERR_INTERNAL;

    /* Exception handler blocks are unreachable via normal CFG edges.
     * Scan for XI_TRY ops and assign RPO numbers to catch targets and
     * all transitively reachable blocks (catch body, finally, merge). */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v->op == XI_TRY && v->aux) {
                XiBlock *catch_blk = (XiBlock *)v->aux;
                /* BFS from catch block to assign RPO to all reachable
                 * exception-related blocks (catch, finally, merge). */
                XiBlock *queue[64];
                int qhead = 0, qtail = 0;
                if (catch_blk->rpo == 0) {
                    catch_blk->rpo = ++rpo_count;
                    queue[qtail++] = catch_blk;
                }
                while (qhead < qtail && qtail < 64) {
                    XiBlock *cur = queue[qhead++];
                    for (int s = 0; s < 2; s++) {
                        XiBlock *succ = cur->succs[s];
                        if (succ && succ->rpo == 0) {
                            succ->rpo = ++rpo_count;
                            queue[qtail++] = succ;
                        }
                    }
                }
            }
        }
    }

    /* Build RPO order array */
    XiBlock **rpo_order = (XiBlock **)xr_calloc(rpo_count + 1,
                                                  sizeof(XiBlock *));
    if (!rpo_order) return XI_EMIT_ERR_INTERNAL;

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (blk->rpo > 0 && blk->rpo <= rpo_count)
            rpo_order[blk->rpo] = blk;
    }

    /* Initialize context */
    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = f;
    ctx.isolate = isolate;
    ctx.proto = xr_vm_proto_new();
    if (!ctx.proto) { xr_free(rpo_order); return XI_EMIT_ERR_INTERNAL; }

    /* Allocate shared variable slots in the isolate's shared_array
     * BEFORE emitting blocks, so child protos can inherit the offset. */
    if (isolate && f->nshared > 0) {
        ctx.proto->shared_offset = isolate->vm.shared.count;
        int total = isolate->vm.shared.count + f->nshared;
        isolate->vm.shared.count = total;
        xr_shared_array_ensure(&isolate->vm.shared, total - 1);
    }

    ctx.rpo_order = rpo_order;
    ctx.rpo_count = rpo_count;

    /* Allocate register map */
    ctx.reg_map_size = f->next_value_id;
    ctx.reg_map = (uint8_t *)xr_malloc(ctx.reg_map_size);
    if (!ctx.reg_map) {
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    memset(ctx.reg_map, NO_REG, ctx.reg_map_size);

    /* Allocate block PC map */
    ctx.block_pc_size = f->next_block_id;
    ctx.block_pc = (int *)xr_malloc(ctx.block_pc_size * sizeof(int));
    if (!ctx.block_pc) {
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    for (uint32_t i = 0; i < ctx.block_pc_size; i++)
        ctx.block_pc[i] = -1;

    /* Allocate last-use ordinal map for register recycling */
    ctx.last_use = (uint32_t *)xr_calloc(ctx.reg_map_size, sizeof(uint32_t));
    if (!ctx.last_use) {
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    compute_last_use(&ctx);

    /* Allocate cell-wrapped tracker for dedup of OP_CELL_NEW */
    ctx.cell_wrapped = (bool *)xr_calloc(ctx.reg_map_size, sizeof(bool));
    if (!ctx.cell_wrapped) {
        xr_free(ctx.last_use);
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }

    /* Allocate per-value bytecode PC map for IC-guided JIT speculation */
    ctx.value_pc = (int *)xr_malloc(ctx.reg_map_size * sizeof(int));
    if (!ctx.value_pc) {
        xr_free(ctx.cell_wrapped);
        xr_free(ctx.last_use);
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    for (uint32_t i = 0; i < ctx.reg_map_size; i++)
        ctx.value_pc[i] = -1;

    alloc_registers(&ctx);
    if (ctx.status != XI_EMIT_OK) goto cleanup;

    /* Emit blocks in RPO order */
    for (uint32_t r = 1; r <= rpo_count; r++) {
        XiBlock *blk = rpo_order[r];
        if (!blk) continue;
        XiBlock *next_blk = (r + 1 <= rpo_count) ? rpo_order[r + 1] : NULL;

        /* Before emitting block, emit phi moves from IF predecessors.
         * For PLAIN blocks, phi moves are emitted by the predecessor.
         * For IF predecessors, phi moves for the else path need to be
         * emitted at the else block's start. */
        emit_block(&ctx, blk, next_blk);
        if (ctx.status != XI_EMIT_OK) goto cleanup;
    }

    /* Phase 3: Patch jump targets */
    patch_jumps(&ctx);

    /* Finalize proto metadata */
    ctx.proto->maxstacksize = ctx.max_reg;
    /* Use f->nparams if set by lowerer; otherwise count XI_PARAM ops.
     * Vararg-only functions (e.g. fn(...nums)) have nparams=0 legitimately,
     * so only fall back to counting when nparams==0 and NOT vararg. */
    if (f->nparams > 0 || f->is_vararg) {
        ctx.proto->numparams = f->nparams;
    } else if (f->entry) {
        uint16_t pc = 0;
        for (uint32_t i = 0; i < f->entry->nvalues; i++) {
            if (f->entry->values[i]->op == XI_PARAM) pc++;
        }
        ctx.proto->numparams = pc;
    }
    ctx.proto->is_vararg = f->is_vararg;
    ctx.proto->entry_type = f->entry_type;
    ctx.proto->min_params = f->min_params;

    /* Build slot map for JIT (non-fatal if allocation fails) */
    XiSlotMap *slot_map = build_slot_map(&ctx);
    if (slot_map)
        ctx.proto->xi_slot_map = slot_map;

cleanup:;
    XiEmitStatus result = ctx.status;
    if (result == XI_EMIT_OK) {
        *out_proto = ctx.proto;
    } else {
        xr_vm_proto_free(ctx.proto);
    }
    xr_free(ctx.value_pc);
    xr_free(ctx.cell_wrapped);
    xr_free(ctx.last_use);
    xr_free(ctx.reg_map);
    xr_free(ctx.block_pc);
    xr_free(ctx.patches);
    xr_free(ctx.try_patches);
    xr_free(rpo_order);
    return result;
}

XR_FUNC void xi_emit_attach_ir(struct XrProto *proto, XiFunc *ir) {
    XR_DCHECK(proto != NULL, "xi_emit_attach_ir: NULL proto");
    XR_DCHECK(proto->xi_func == NULL, "xi_emit_attach_ir: proto already has xi_func");
    proto->xi_func = ir;
}

XR_FUNC const char *xi_emit_status_str(XiEmitStatus s) {
    switch (s) {
        case XI_EMIT_OK:                return "OK";
        case XI_EMIT_ERR_TOO_MANY_REGS: return "too many registers (>255)";
        case XI_EMIT_ERR_TOO_MANY_CONSTS: return "constant pool overflow";
        case XI_EMIT_ERR_UNSUPPORTED_OP:  return "unsupported Xi IR operation";
        case XI_EMIT_ERR_INTERNAL:        return "internal emitter error";
    }
    return "unknown error";
}
