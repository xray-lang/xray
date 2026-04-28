/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit.c - Xi IR to VM bytecode emitter
 *
 * Translates typed SSA IR (XiFunc) into register-based bytecode
 * targeting the existing Xray VM (XrProto / xchunk.h format).
 */

#include "xi_emit.h"
#include "xi_analysis.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/object/xstring.h"
#include "../../include/xray_isolate.h"
#include <string.h>

/* ========== Emit Context ========== */

#define MAX_REGS 256
#define NO_REG   255

typedef struct {
    XiFunc *func;
    XrProto *proto;
    XrayIsolate *isolate;      /* for string interning; may be NULL */
    XiEmitStatus status;

    /* Register allocation: value_id -> register number */
    uint8_t *reg_map;        /* [next_value_id] */
    uint32_t reg_map_size;
    uint8_t next_reg;        /* next free register */
    uint8_t max_reg;         /* high-water mark */

    /* Free register stack for register recycling */
    uint8_t free_regs[MAX_REGS];
    uint16_t nfree;          /* count of free registers on the stack */

    /* Liveness: per-value last-use tracking (value_id -> last-use ordinal) */
    uint32_t *last_use;      /* [next_value_id], 0 = unused/dead */
    uint32_t current_ordinal;/* monotonic instruction counter */

    /* Line number tracking for debug info */
    int current_line;        /* line of the value being emitted */

    /* Block linearization */
    XiBlock **rpo_order;     /* blocks in RPO order */
    uint32_t rpo_count;

    /* Jump patching: block_id -> start PC */
    int *block_pc;           /* [next_block_id], -1 = not yet emitted */
    uint32_t block_pc_size;

    /* Pending jump patches: instructions that need target PCs */
    struct {
        int pc;              /* instruction PC to patch */
        uint32_t target_bid; /* target block ID */
    } *patches;
    uint32_t npatch;
    uint32_t patch_cap;
} EmitCtx;

/* ========== Helpers ========== */

static void emit_error(EmitCtx *ctx, XiEmitStatus s) {
    if (ctx->status == XI_EMIT_OK)
        ctx->status = s;
}

static int current_pc(EmitCtx *ctx) {
    return PROTO_CODE_COUNT(ctx->proto);
}

static void emit_inst(EmitCtx *ctx, XrInstruction inst) {
    xr_vm_proto_write(ctx->proto, inst, ctx->current_line);
}

/* Return a register to the free pool for reuse. */
static void free_reg(EmitCtx *ctx, uint8_t reg) {
    if (reg == NO_REG) return;
    if (ctx->nfree < MAX_REGS) {
        ctx->free_regs[ctx->nfree++] = reg;
    }
}

/* Get register for a value. Assigns one if not yet mapped.
 * Uses free register stack before allocating new ones. */
static uint8_t reg_of(EmitCtx *ctx, const XiValue *v) {
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
static uint8_t alloc_reg_fresh(EmitCtx *ctx, const XiValue *v) {
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
static void try_free_args(EmitCtx *ctx, const XiValue *v) {
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

/* Add constant to pool, return index. */
static int add_const_int(EmitCtx *ctx, int64_t val) {
    XrValue xv = xr_make_int_val(val, XR_TAG_I64);
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

static int add_const_float(EmitCtx *ctx, double val) {
    XrValue xv = xr_make_float_val(val, XR_TAG_F64);
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

static int add_const_string(EmitCtx *ctx, const char *str) {
    XrValue xv;
    if (ctx->isolate && str) {
        XrString *xs = xr_compile_time_intern(ctx->isolate, str, strlen(str));
        xv = xr_string_value(xs);
    } else {
        xv = xr_make_ptr_val((void *)str);
    }
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
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

/* ========== Instruction Selection ========== */

static void emit_value(EmitCtx *ctx, XiValue *v) {
    if (ctx->status != XI_EMIT_OK) return;

    /* Call-like ops place args at dst+1..dst+nargs.  A recycled low register
     * for dst could overlap with live source arg registers (clobber bug).
     * Force fresh allocation so dst > all previously allocated registers. */
    bool call_like = (v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_GO);
    uint8_t dst = call_like ? alloc_reg_fresh(ctx, v) : reg_of(ctx, v);
    if (ctx->status != XI_EMIT_OK) return;

    switch (v->op) {
        case XI_CONST: {
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
                default:
                    emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
                    return;
            }
            break;
        }

        case XI_PARAM:
            /* Params already in registers; no-op. */
            break;

        case XI_COPY: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            if (dst != src)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
            break;
        }

        /* Binary arithmetic — with instruction fusion for constant operands.
         * ADDI/SUBI/MULI use signed 8-bit immediate (int8_t, -128..127).
         * ADDK/SUBK/MULK/DIVK use constant pool index. */
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_SHL: case XI_SHR: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];

            /* Try fused immediate form: OP_ADDI/SUBI/MULI with signed 8-bit C */
            bool rhs_is_small_int = (rhs->op == XI_CONST && rhs->type &&
                                     rhs->type->kind == XR_KIND_INT &&
                                     rhs->aux_int >= -128 && rhs->aux_int <= 127);
            bool lhs_is_small_int = (lhs->op == XI_CONST && lhs->type &&
                                     lhs->type->kind == XR_KIND_INT &&
                                     lhs->aux_int >= -128 && lhs->aux_int <= 127);

            if (rhs_is_small_int &&
                (v->op == XI_ADD || v->op == XI_SUB || v->op == XI_MUL)) {
                uint8_t b = reg_of(ctx, lhs);
                if (ctx->status != XI_EMIT_OK) return;
                int8_t imm = (int8_t)rhs->aux_int;
                OpCode fused = v->op == XI_ADD ? OP_ADDI :
                               v->op == XI_SUB ? OP_SUBI : OP_MULI;
                emit_inst(ctx, CREATE_ABC(fused, dst, b, (uint8_t)imm));
                break;
            }

            /* ADD is commutative: try swapping if lhs is the constant */
            if (lhs_is_small_int && v->op == XI_ADD) {
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK) return;
                int8_t imm = (int8_t)lhs->aux_int;
                emit_inst(ctx, CREATE_ABC(OP_ADDI, dst, b, (uint8_t)imm));
                break;
            }
            /* MUL is commutative: try swapping if lhs is the constant */
            if (lhs_is_small_int && v->op == XI_MUL) {
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK) return;
                int8_t imm = (int8_t)lhs->aux_int;
                emit_inst(ctx, CREATE_ABC(OP_MULI, dst, b, (uint8_t)imm));
                break;
            }

            /* Try constant-pool form: ADDK/SUBK/MULK/DIVK for larger constants */
            bool rhs_is_const_num = (rhs->op == XI_CONST && rhs->type &&
                (rhs->type->kind == XR_KIND_INT || rhs->type->kind == XR_KIND_FLOAT));
            if (rhs_is_const_num && !rhs_is_small_int &&
                (v->op == XI_ADD || v->op == XI_SUB ||
                 v->op == XI_MUL || v->op == XI_DIV)) {
                uint8_t b = reg_of(ctx, lhs);
                if (ctx->status != XI_EMIT_OK) return;
                int ki;
                if (rhs->type->kind == XR_KIND_INT) {
                    ki = add_const_int(ctx, rhs->aux_int);
                } else {
                    double fval;
                    memcpy(&fval, &rhs->aux_int, sizeof(double));
                    ki = add_const_float(ctx, fval);
                }
                if (ctx->status != XI_EMIT_OK) return;
                OpCode kop = v->op == XI_ADD ? OP_ADDK :
                             v->op == XI_SUB ? OP_SUBK :
                             v->op == XI_MUL ? OP_MULK : OP_DIVK;
                emit_inst(ctx, CREATE_ABC(kop, dst, b, (uint8_t)ki));
                break;
            }

            /* Generic register-register form */
            uint8_t b = reg_of(ctx, lhs);
            uint8_t c = reg_of(ctx, rhs);
            if (ctx->status != XI_EMIT_OK) return;

            OpCode op;
            switch (v->op) {
                case XI_ADD:  op = OP_ADD;  break;
                case XI_SUB:  op = OP_SUB;  break;
                case XI_MUL:  op = OP_MUL;  break;
                case XI_DIV:  op = OP_DIV;  break;
                case XI_MOD:  op = OP_MOD;  break;
                case XI_BAND: op = OP_BAND; break;
                case XI_BOR:  op = OP_BOR;  break;
                case XI_BXOR: op = OP_BXOR; break;
                case XI_SHL:  op = OP_SHL;  break;
                case XI_SHR:  op = OP_SHR;  break;
                default: op = OP_NOP; break;
            }
            emit_inst(ctx, CREATE_ABC(op, dst, b, c));
            break;
        }

        /* Unary ops */
        case XI_NEG:
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            emit_inst(ctx, CREATE_ABC(OP_UNM, dst, reg_of(ctx, v->args[0]), 0));
            break;
        case XI_NOT:
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            emit_inst(ctx, CREATE_ABC(OP_NOT, dst, reg_of(ctx, v->args[0]), 0));
            break;
        case XI_BNOT:
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            emit_inst(ctx, CREATE_ABC(OP_BNOT, dst, reg_of(ctx, v->args[0]), 0));
            break;

        /* Comparison ops -> CMP_* (produce bool in register) */
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE:
        case XI_GT: case XI_GE: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t b = reg_of(ctx, v->args[0]);
            uint8_t c = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;

            switch (v->op) {
                case XI_EQ: emit_inst(ctx, CREATE_ABC(OP_CMP_EQ, dst, b, c)); break;
                case XI_NE: emit_inst(ctx, CREATE_ABC(OP_CMP_NE, dst, b, c)); break;
                case XI_LT: emit_inst(ctx, CREATE_ABC(OP_CMP_LT, dst, b, c)); break;
                case XI_LE: emit_inst(ctx, CREATE_ABC(OP_CMP_LE, dst, b, c)); break;
                /* GT/GE: swap args */
                case XI_GT: emit_inst(ctx, CREATE_ABC(OP_CMP_LT, dst, c, b)); break;
                case XI_GE: emit_inst(ctx, CREATE_ABC(OP_CMP_LE, dst, c, b)); break;
                default: break;
            }
            break;
        }

        /* Type conversion */
        case XI_CONVERT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            struct XrType *target = v->type;
            if (!target) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            switch (target->kind) {
                case XR_KIND_INT:   emit_inst(ctx, CREATE_ABC(OP_TOINT, dst, src, 0)); break;
                case XR_KIND_FLOAT: emit_inst(ctx, CREATE_ABC(OP_TOFLOAT, dst, src, 0)); break;
                case XR_KIND_STRING:emit_inst(ctx, CREATE_ABC(OP_TOSTRING, dst, src, 0)); break;
                case XR_KIND_BOOL:  emit_inst(ctx, CREATE_ABC(OP_TOBOOL, dst, src, 0)); break;
                default: emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP); return;
            }
            break;
        }

        /* Memory: field access */
        case XI_LOAD_FIELD: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int field_idx = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_GETFIELD, dst, obj, (uint8_t)field_idx));
            break;
        }
        case XI_STORE_FIELD: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            uint8_t val = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            int field_idx = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_SETFIELD, obj, (uint8_t)field_idx, val));
            break;
        }

        /* Indexing */
        case XI_INDEX_GET: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            uint8_t key = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_INDEX_GET, dst, obj, key));
            break;
        }
        case XI_INDEX_SET: {
            if (v->nargs < 3) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            uint8_t key = reg_of(ctx, v->args[1]);
            uint8_t val = reg_of(ctx, v->args[2]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_INDEX_SET, obj, key, val));
            break;
        }

        /* Print */
        case XI_PRINT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int flags = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_PRINT, src, (uint8_t)(flags & 1),
                                      (uint8_t)((flags >> 1) & 0xFF)));
            break;
        }

        /* Function calls */
        case XI_CALL: {
            /* args[0]=callee, args[1..n]=params */
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t nargs = (uint8_t)(v->nargs - 1);
            bool self_call = (v->aux_int == 1);

            /* Account for arg registers (dst+1..dst+nargs) in maxstacksize */
            {
                uint8_t call_top = (uint8_t)(dst + nargs + 1);
                if (call_top > ctx->max_reg) ctx->max_reg = call_top;
            }

            if (self_call) {
                /* Recursive self-call: OP_CALLSELF uses frame->closure,
                 * no callee register needed. Use dst as base. */
                for (uint16_t a = 1; a < v->nargs; a++) {
                    uint8_t arg_reg = reg_of(ctx, v->args[a]);
                    if (ctx->status != XI_EMIT_OK) return;
                    uint8_t target = (uint8_t)(dst + a);
                    if (arg_reg != target) {
                        emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
                    }
                }
                emit_inst(ctx, CREATE_ABC(OP_CALLSELF, dst, nargs, 1));
            } else {
                /* Use dst as call base: OP_CALL writes result to the base
                 * register, so using the callee's original register would
                 * clobber the closure (breaks nested calls to the same fn).
                 * dst is always beyond all source registers, so copying
                 * callee→dst and args→dst+1..dst+n is safe. */
                uint8_t callee = reg_of(ctx, v->args[0]);
                if (ctx->status != XI_EMIT_OK) return;
                if (callee != dst) {
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));
                }
                for (uint16_t a = 1; a < v->nargs; a++) {
                    uint8_t arg_reg = reg_of(ctx, v->args[a]);
                    if (ctx->status != XI_EMIT_OK) return;
                    uint8_t target = (uint8_t)(dst + a);
                    if (arg_reg != target) {
                        emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
                    }
                }
                emit_inst(ctx, CREATE_ABC(OP_CALL, dst, nargs, 1));
            }
            break;
        }

        /* Containers */
        case XI_ARRAY_NEW: {
            uint8_t cap = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                cap = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABC(OP_NEWARRAY, dst, cap, 0));
            break;
        }
        case XI_MAP_NEW: {
            uint8_t cap = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                cap = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, cap, 0));
            break;
        }

        /* Throw */
        case XI_THROW: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_THROW, src, 0, 0));
            break;
        }

        /* Box/Unbox */
        case XI_BOX: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            /* Use BOX_I64 for ints, BOX_F64 for floats; default to move */
            struct XrType *sty = v->args[0]->type;
            if (sty && sty->kind == XR_KIND_FLOAT)
                emit_inst(ctx, CREATE_ABC(OP_BOX_F64, dst, src, 0));
            else
                emit_inst(ctx, CREATE_ABC(OP_BOX_I64, dst, src, 0));
            break;
        }
        case XI_UNBOX: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            struct XrType *dty = v->type;
            if (dty && dty->kind == XR_KIND_FLOAT)
                emit_inst(ctx, CREATE_ABC(OP_UNBOX_F64, dst, src, 0));
            else
                emit_inst(ctx, CREATE_ABC(OP_UNBOX_I64, dst, src, 0));
            break;
        }

        /* Null check */
        case XI_ISNULL: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_ISNULL_SET, dst, src, 0));
            break;
        }

        /* Coroutine */
        case XI_GO: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t callee = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t nargs = (uint8_t)(v->nargs - 1);
            emit_inst(ctx, CREATE_ABC(OP_GO, dst, callee, nargs));
            break;
        }
        case XI_AWAIT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t task = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_AWAIT, dst, task, 0));
            break;
        }
        case XI_YIELD:
            emit_inst(ctx, CREATE_ABC(OP_YIELD, 0, 0, 0));
            break;
        case XI_CHAN_NEW: {
            uint8_t buf = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                buf = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABx(OP_CHAN_NEW, dst, buf));
            break;
        }
        case XI_CHAN_SEND: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t ch = reg_of(ctx, v->args[0]);
            uint8_t val = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_CHAN_SEND, 0, ch, val));
            break;
        }
        case XI_CHAN_RECV: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t ch = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_CHAN_RECV, dst, ch, 0));
            break;
        }

        /* Iteration */
        case XI_RANGE: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t start = reg_of(ctx, v->args[0]);
            uint8_t end = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_NEWRANGE, dst, start, end));
            break;
        }

        /* Slice: OP_SLICE expects start at R[C], end at R[C+1] (consecutive). */
        case XI_SLICE: {
            if (v->nargs < 3) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src    = reg_of(ctx, v->args[0]);
            uint8_t lo_src = reg_of(ctx, v->args[1]);
            uint8_t hi_src = reg_of(ctx, v->args[2]);
            if (ctx->status != XI_EMIT_OK) return;
            /* Allocate consecutive temp pair for start/end */
            if (ctx->next_reg + 2 >= MAX_REGS) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS); return;
            }
            uint8_t lo_slot = ctx->next_reg;
            ctx->next_reg += 2;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
            emit_inst(ctx, CREATE_ABC(OP_MOVE, lo_slot, lo_src, 0));
            emit_inst(ctx, CREATE_ABC(OP_MOVE, (uint8_t)(lo_slot + 1), hi_src, 0));
            emit_inst(ctx, CREATE_ABC(OP_SLICE, dst, src, lo_slot));
            break;
        }

        /* Closure creation: recursively emit child XiFunc, register sub-proto,
         * then emit OP_CLOSURE(A, Bx=proto_index). */
        case XI_CLOSURE_NEW: {
            XiFunc *child_func = (XiFunc *)v->aux;
            XR_DCHECK(child_func != NULL, "closure child func must not be NULL");

            XrProto *child_proto = NULL;
            XiEmitStatus child_st = xi_emit(child_func, ctx->isolate, &child_proto);
            if (child_st != XI_EMIT_OK || !child_proto) {
                emit_error(ctx, child_st != XI_EMIT_OK
                           ? child_st : XI_EMIT_ERR_INTERNAL);
                return;
            }
            int proto_idx = xr_vm_proto_add_proto(ctx->proto, child_proto);
            emit_inst(ctx, CREATE_ABx(OP_CLOSURE, dst, proto_idx));
            break;
        }

        /* Upvalue access */
        case XI_LOAD_UPVAL: {
            int upval_idx = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, dst, (uint8_t)upval_idx, 0));
            break;
        }
        case XI_STORE_UPVAL: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t val = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int upval_idx = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_CELL_SET, (uint8_t)upval_idx, val, 0));
            break;
        }

        /* Method call: args[0]=receiver, args[1..n]=params, aux=method name */
        case XI_CALL_METHOD: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t recv = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t nargs = (uint8_t)(v->nargs - 1);

            /* Place receiver and args in consecutive registers */
            for (uint16_t a = 1; a < v->nargs; a++) {
                uint8_t arg_reg = reg_of(ctx, v->args[a]);
                if (ctx->status != XI_EMIT_OK) return;
                uint8_t target = (uint8_t)(recv + a);
                if (arg_reg != target)
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }

            /* Method name -> constant pool -> INVOKE */
            const char *method_name = (const char *)v->aux;
            int ki = add_const_string(ctx, method_name);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_INVOKE, recv, (uint8_t)ki, nargs + 1));

            if (dst != recv)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, recv, 0));
            break;
        }

        /* Builtin call: aux_int=builtin_id */
        case XI_CALL_BUILTIN: {
            int builtin_id = (int)v->aux_int;
            /* Special case: builtin 0 = cancelled() */
            if (builtin_id == 0) {
                emit_inst(ctx, CREATE_ABC(OP_CANCELLED, dst, 0, 0));
            } else {
                /* Generic: INVOKE_BUILTIN A=base, B=builtin_idx, C=nargs */
                if (v->nargs > 0) {
                    uint8_t base = reg_of(ctx, v->args[0]);
                    if (ctx->status != XI_EMIT_OK) return;
                    emit_inst(ctx, CREATE_ABC(OP_INVOKE_BUILTIN,
                                              base, (uint8_t)builtin_id,
                                              (uint8_t)v->nargs));
                    if (dst != base)
                        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
                }
            }
            break;
        }

        /* String concatenation: STRBUF_NEW + STRBUF_APPEND*n + STRBUF_FINISH */
        case XI_STR_CONCAT: {
            emit_inst(ctx, CREATE_ABC(OP_STRBUF_NEW, dst, 0, 0));
            for (uint16_t a = 0; a < v->nargs; a++) {
                uint8_t part = reg_of(ctx, v->args[a]);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_STRBUF_APPEND, dst, part, 0));
            }
            emit_inst(ctx, CREATE_ABC(OP_STRBUF_FINISH, dst, 0, 0));
            break;
        }

        /* Object allocation: aux=class/type name */
        case XI_ALLOC: {
            /* Emit as NEWMAP with field capacity from aux or args[0] */
            uint8_t cap = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                cap = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, cap, 0));
            break;
        }

        /* Set creation */
        case XI_SET_NEW: {
            uint8_t cap = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                cap = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABC(OP_NEWSET, dst, cap, 0));
            break;
        }

        /* Defer: args[0]=callee; OP_DEFER A=callee_reg B=nargs */
        case XI_DEFER: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t callee = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t nargs = (uint8_t)(v->nargs > 1 ? v->nargs - 1 : 0);
            emit_inst(ctx, CREATE_ABC(OP_DEFER, callee, nargs, 0));
            break;
        }

        /* Type check: IS A B C — R[A] = (R[B] is Type[C]) */
        case XI_IS: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int type_id = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_IS, dst, src, (uint8_t)type_id));
            break;
        }

        /* Type cast: treated as assertion + move */
        case XI_AS: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            /* For now, cast is just a move (runtime check happens via IS) */
            if (dst != src)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
            break;
        }

        default:
            emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
            return;
    }
}

/* ========== Block Emission ========== */

static void emit_block(EmitCtx *ctx, XiBlock *blk, XiBlock *next_blk) {
    if (ctx->status != XI_EMIT_OK) return;

    /* Record block start PC */
    XR_DCHECK(blk->id < ctx->block_pc_size, "block_id out of range");
    ctx->block_pc[blk->id] = current_pc(ctx);

    /* Emit instruction values with register recycling */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        ctx->current_ordinal++;
        ctx->current_line = (int)blk->values[i]->line;
        emit_value(ctx, blk->values[i]);
        if (ctx->status != XI_EMIT_OK) return;
        /* Recycle registers of args whose last use was this instruction */
        if (ctx->last_use)
            try_free_args(ctx, blk->values[i]);
    }

    ctx->current_ordinal++;  /* ordinal for terminator */
    /* Emit terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN:
            if (blk->control) {
                uint8_t r = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK) return;
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
            uint8_t cond = reg_of(ctx, blk->control);
            if (ctx->status != XI_EMIT_OK) return;

            XiBlock *then_b = blk->succs[0];
            XiBlock *else_b = blk->succs[1];
            XR_DCHECK(then_b && else_b, "IF block missing successor");

            /* TEST cond, 0 — skip next instruction if cond is false */
            emit_inst(ctx, CREATE_ABC(OP_TEST, cond, 0, 0));

            /* JMP -> else block (if TEST fails, i.e., cond is false) */
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
    /* Count params from entry block scan or f->nparams */
    uint16_t param_count = 0;
    if (f->entry) {
        for (uint32_t i = 0; i < f->entry->nvalues; i++) {
            if (f->entry->values[i]->op == XI_PARAM)
                param_count++;
        }
    }
    ctx.proto->numparams = param_count;

cleanup:;
    XiEmitStatus result = ctx.status;
    if (result == XI_EMIT_OK) {
        *out_proto = ctx.proto;
    } else {
        xr_vm_proto_free(ctx.proto);
    }
    xr_free(ctx.last_use);
    xr_free(ctx.reg_map);
    xr_free(ctx.block_pc);
    xr_free(ctx.patches);
    xr_free(rpo_order);
    return result;
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
