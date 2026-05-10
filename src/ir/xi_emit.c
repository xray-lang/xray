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
#include "../runtime/object/xbigint.h"
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
    if (reg == NO_REG)
        return;
    if (ctx->nfree < MAX_REGS) {
        ctx->free_regs[ctx->nfree++] = reg;
    }
}

/* Get register for a value. Assigns one if not yet mapped.
 * Uses free register stack before allocating new ones.
 * Values annotated with a source var_id are coalesced to share
 * the same register, which is necessary for correct exception
 * handling where OP_THROW bypasses SSA phi resolution. */
XR_FUNC uint8_t reg_of(EmitCtx *ctx, const XiValue *v) {
    XR_DCHECK(v != NULL, "reg_of: NULL value");

    if (v->id >= ctx->reg_map_size) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return 0;
    }

    if (ctx->reg_map[v->id] == NO_REG) {
        /* Variable coalescing: reuse the pinned register for this var_id */
        if (v->var_id != 0xFF && ctx->var_reg[v->var_id] != NO_REG) {
            ctx->reg_map[v->id] = ctx->var_reg[v->var_id];
            return ctx->reg_map[v->id];
        }
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
        /* Record as the pinned register for this variable */
        if (v->var_id != 0xFF)
            ctx->var_reg[v->var_id] = ctx->reg_map[v->id];
    }
    return ctx->reg_map[v->id];
}

/* Like reg_of, but if the value's var_id maps to a cell-wrapped register,
 * emit OP_CELL_GET to a temporary and return that.  Use this for reads
 * where the raw register might hold a cell instead of the actual value
 * (e.g. calling a hoisted function, reading a mutable captured variable). */
XR_FUNC uint8_t reg_of_cell_deref(EmitCtx *ctx, const XiValue *v) {
    uint8_t r = reg_of(ctx, v);
    if (ctx->status != XI_EMIT_OK)
        return r;
    if (v->var_id != 0xFF && ctx->cell_side_reg[v->var_id] != NO_REG) {
        uint8_t tmp = ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        emit_inst(ctx, CREATE_ABC(OP_CELL_GET, tmp, r, 0));
        return tmp;
    }
    return r;
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
 * Called AFTER emitting an instruction that reads these args.
 * Coalesced registers (var_id != 0xFF) are never freed — they must remain
 * pinned so all SSA definitions of the variable share one VM register. */
XR_FUNC void try_free_args(EmitCtx *ctx, const XiValue *v) {
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg->id >= ctx->reg_map_size)
            continue;
        if (arg->var_id != 0xFF)
            continue; /* pinned by coalescing */
        /* Free register if this is the last use of arg */
        if (ctx->last_use[arg->id] == ctx->current_ordinal) {
            uint8_t r = ctx->reg_map[arg->id];
            ctx->reg_map[arg->id] = NO_REG;
            free_reg(ctx, r);
        }
    }
}

/* Add a pending jump patch. */
XR_FUNC void xi_emit_add_patch(EmitCtx *ctx, int pc, uint32_t target_bid) {
    if (ctx->npatch >= ctx->patch_cap) {
        uint32_t new_cap = ctx->patch_cap ? ctx->patch_cap * 2 : 16;
        void *tmp = xr_realloc(ctx->patches, new_cap * sizeof(ctx->patches[0]));
        if (!tmp) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        ctx->patches = tmp;
        ctx->patch_cap = new_cap;
    }
    ctx->patches[ctx->npatch].pc = pc;
    ctx->patches[ctx->npatch].target_bid = target_bid;
    ctx->npatch++;
}

/* Add a pending OP_TRY patch (catch + finally absolute PC targets). */
XR_FUNC void add_try_patch(EmitCtx *ctx, int pc, uint32_t catch_bid, uint32_t finally_bid) {
    if (ctx->ntry_patch >= ctx->try_patch_cap) {
        uint32_t new_cap = ctx->try_patch_cap ? ctx->try_patch_cap * 2 : 4;
        void *tmp = xr_realloc(ctx->try_patches, new_cap * sizeof(ctx->try_patches[0]));
        if (!tmp) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
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
    XrSymbolTable *st = (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->isolate);
    XR_DCHECK(st != NULL, "isolate must have a symbol table");
    SymbolId global = xr_symbol_register_in_table(st, name);
    int local = xr_proto_add_symbol(ctx->proto, (int32_t) global);
    if (local > MAXARG_B) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
        return -1;
    }
    return local;
}

/* Register allocation, liveness, phi elimination are in xi_emit_reg.c.
 * Block emission, jump patching are in xi_emit_cf.c.
 * Class helpers are in xi_emit_object.c.
 * Slot map generation is in xi_emit_slotmap.c. */

/* ========== Local Handlers (CONST, PARAM, COPY, SELECT) ========== */

static void emit_const(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    struct XrType *ty = v->type;
    if (!ty) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    switch (ty->kind) {
        case XR_KIND_INT: {
            int64_t val = v->aux_int;
            if (val >= LOADI_MIN && val <= LOADI_MAX) {
                emit_inst(ctx, CREATE_AsBx(OP_LOADI, dst, (int) val));
            } else {
                int ki = add_const_int(ctx, val);
                if (ctx->status != XI_EMIT_OK)
                    return;
                emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            }
            break;
        }
        case XR_KIND_FLOAT: {
            double fval;
            memcpy(&fval, &v->aux_int, sizeof(double));
            int sv = (int) fval;
            if ((double) sv == fval && sv >= LOADI_MIN && sv <= LOADI_MAX) {
                emit_inst(ctx, CREATE_AsBx(OP_LOADF, dst, sv));
            } else {
                int ki = add_const_float(ctx, fval);
                if (ctx->status != XI_EMIT_OK)
                    return;
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
            const char *s = (const char *) v->aux;
            int ki = add_const_string(ctx, s);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            break;
        }
        case XR_KIND_INSTANCE: {
            /* BigInt: aux holds decimal digit string, create XrBigInt object */
            if (xr_type_is_named_class(ty, "BigInt") && v->aux) {
                const char *digits = (const char *) v->aux;
                XrBigInt *bi = xr_bigint_from_string_on_gc(&ctx->isolate->gc, digits);
                if (!bi) {
                    emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                    return;
                }
                XrValue xv = XR_FROM_PTR(bi);
                int ki = xr_vm_proto_add_constant(ctx->proto, xv);
                if (ki > MAXARG_Bx) {
                    emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
                    return;
                }
                emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
                break;
            }
            /* Other instance constants: fall through to default */
        }
        /* fall through */
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
    (void) ctx;
    (void) v;
    (void) dst;
    /* Params already in registers; no-op. */
}

static void emit_copy(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    /* Value types (structs) need deep copy to maintain copy-on-assign semantics */
    XrType *src_type = v->args[0]->type;
    if (src_type && src_type->is_value_type) {
        emit_inst(ctx, CREATE_ABC(OP_COPY, dst, src, 0));
    } else if (dst != src) {
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
    }
}

/* Conditional select: dst = cond ? true_val : false_val.
 * Emitted as: MOVE dst, false_val; TEST cond, 0; MOVE dst, true_val.
 * TEST skips the next instruction when cond is falsy. */
static void emit_select(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t cond_r = reg_of(ctx, v->args[0]);
    uint8_t true_r = reg_of(ctx, v->args[1]);
    uint8_t false_r = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (dst != false_r)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, false_r, 0));
    emit_inst(ctx, CREATE_ABC(OP_TEST, cond_r, 0, 0));
    if (dst != true_r)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, true_r, 0));
}

/* ========== Dispatch Table ========== */

const XiEmitHandler xi_emit_handlers[XI_OP_COUNT] = {
    [XI_CONST] = emit_const,
    [XI_PARAM] = emit_param,
    [XI_ADD] = xi_emit_arith,
    [XI_SUB] = xi_emit_arith,
    [XI_MUL] = xi_emit_arith,
    [XI_DIV] = xi_emit_arith,
    [XI_MOD] = xi_emit_arith,
    [XI_NEG] = xi_emit_neg,
    [XI_BAND] = xi_emit_arith,
    [XI_BOR] = xi_emit_arith,
    [XI_BXOR] = xi_emit_arith,
    [XI_BNOT] = xi_emit_bnot,
    [XI_SHL] = xi_emit_arith,
    [XI_SHR] = xi_emit_arith,
    [XI_EQ] = xi_emit_cmp,
    [XI_NE] = xi_emit_cmp,
    [XI_LT] = xi_emit_cmp,
    [XI_LE] = xi_emit_cmp,
    [XI_GT] = xi_emit_cmp,
    [XI_GE] = xi_emit_cmp,
    [XI_EQ_STRICT] = xi_emit_cmp,
    [XI_NE_STRICT] = xi_emit_cmp,
    [XI_NOT] = xi_emit_not,
    [XI_CONVERT] = xi_emit_convert,
    [XI_BOX] = xi_emit_box,
    [XI_UNBOX] = xi_emit_unbox,
    [XI_NARROW_I8] = xi_emit_narrow,
    [XI_NARROW_U8] = xi_emit_narrow,
    [XI_NARROW_I16] = xi_emit_narrow,
    [XI_NARROW_U16] = xi_emit_narrow,
    [XI_NARROW_I32] = xi_emit_narrow,
    [XI_NARROW_U32] = xi_emit_narrow,
    [XI_NARROW_F32] = xi_emit_narrow,
    [XI_WIDEN_I8] = xi_emit_widen,
    [XI_WIDEN_U8] = xi_emit_widen,
    [XI_WIDEN_I16] = xi_emit_widen,
    [XI_WIDEN_U16] = xi_emit_widen,
    [XI_WIDEN_I32] = xi_emit_widen,
    [XI_WIDEN_U32] = xi_emit_widen,
    [XI_WIDEN_F32] = xi_emit_widen,
    [XI_LOAD_FIELD] = xi_emit_load_field,
    [XI_STORE_FIELD] = xi_emit_store_field,
    [XI_STRUCT_NEW] = xi_emit_struct_new,
    [XI_STRUCT_GET] = xi_emit_struct_get,
    [XI_STRUCT_SET] = xi_emit_struct_set,
    [XI_INDEX_GET] = xi_emit_index_get,
    [XI_INDEX_SET] = xi_emit_index_set,
    [XI_JSON_NEW] = xi_emit_json_new,
    [XI_JSON_INIT_F] = xi_emit_json_init_f,
    [XI_JSON_GET_F] = xi_emit_json_get_f,
    [XI_JSON_SET_F] = xi_emit_json_set_f,
    [XI_JSON_DECODE] = xi_emit_json_decode,
    [XI_ARRAY_NEW] = xi_emit_array_new,
    [XI_MAP_NEW] = xi_emit_map_new,
    [XI_CALL] = xi_emit_call,
    [XI_CALL_METHOD] = xi_emit_call_method,
    [XI_CALL_BUILTIN] = xi_emit_call_builtin,
    [XI_EXTRACT] = xi_emit_extract,
    [XI_CLOSURE_NEW] = xi_emit_closure_new,
    [XI_LOAD_UPVAL] = xi_emit_load_upval,
    [XI_STORE_UPVAL] = xi_emit_store_upval,
    [XI_GET_SHARED] = xi_emit_get_shared,
    [XI_SET_SHARED] = xi_emit_set_shared,
    [XI_PRINT] = xi_emit_print,
    [XI_GO] = xi_emit_go,
    [XI_AWAIT] = xi_emit_await,
    [XI_CHAN_SEND] = xi_emit_chan_send,
    [XI_CHAN_RECV] = xi_emit_chan_recv,
    [XI_CHAN_TRY_SEND] = xi_emit_chan_try_send,
    [XI_CHAN_TRY_RECV] = xi_emit_chan_try_recv,
    [XI_YIELD] = xi_emit_yield,
    [XI_THROW] = xi_emit_throw,
    [XI_ITER_NEW] = xi_emit_iter,
    [XI_ITER_NEXT] = xi_emit_iter,
    [XI_ITER_VALID] = xi_emit_iter,
    [XI_DEFER] = xi_emit_defer,
    [XI_CHAN_NEW] = xi_emit_chan_new,
    [XI_CORO_OP] = xi_emit_coro_op,
    [XI_SET_NEW] = xi_emit_set_new,
    [XI_STR_CONCAT] = xi_emit_str_concat,
    [XI_IS] = xi_emit_is,
    [XI_AS] = xi_emit_as,
    [XI_SLICE] = xi_emit_slice,
    [XI_RANGE] = xi_emit_range,
    [XI_MULTI_RET] = xi_emit_multi_ret,
    [XI_ISNULL] = xi_emit_isnull,
    [XI_PHI] = NULL, /* handled separately by emit_phi_moves */
    [XI_SELECT] = emit_select,
    [XI_COPY] = emit_copy,
    [XI_CLASS_CREATE] = xi_emit_class_create,
    [XI_SCOPE_ENTER] = xi_emit_scope_enter,
    [XI_SCOPE_EXIT] = xi_emit_scope_exit,
    [XI_TRY] = xi_emit_try,
    [XI_CATCH] = xi_emit_catch,
    [XI_FINALLY] = xi_emit_finally,
    [XI_END_TRY] = xi_emit_end_try,
    [XI_ASSERT] = xi_emit_assert,
    [XI_ASSERT_EQ] = xi_emit_assert_eq,
    [XI_ASSERT_NE] = xi_emit_assert_ne,
    [XI_ASSERT_THROWS] = xi_emit_assert_throws,
    [XI_TYPEOF] = xi_emit_typeof,
    [XI_GET_BUILTIN] = xi_emit_get_builtin,
    [XI_IMPORT_REF] = xi_emit_import_ref,
    [XI_REGEX_COMPILE] = xi_emit_regex_compile,
};

/* ========== Instruction Selection ========== */

XR_FUNC void emit_value(EmitCtx *ctx, XiValue *v) {
    if (ctx->status != XI_EMIT_OK)
        return;

    /* Skip comparison that was absorbed into the block terminator */
    if (v == ctx->fused_cmp)
        return;

    /* Call-like ops place args at dst+1..dst+nargs.  A recycled low register
     * for dst could overlap with live source arg registers (clobber bug).
     * Force fresh allocation so dst > all previously allocated registers. */
    /* OP_CHAN_TRY_RECV clobbers R[dst+1] with the ok flag — treat it as
     * call-like so dst is beyond all live registers. */
    bool call_like = (v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_GO ||
                      v->op == XI_MULTI_RET || v->op == XI_CHAN_TRY_RECV);
    uint8_t dst = call_like ? alloc_reg_fresh(ctx, v) : reg_of(ctx, v);
    if (ctx->status != XI_EMIT_OK)
        return;

/* Cell unwrapping: when an arg was wrapped in a cell for mutable closure
 * capture, reading its register returns the XrCell object, not the value.
 * Emit CELL_GET to a temp register for each such arg and temporarily
 * redirect reg_map so the handler sees the unwrapped value.
 * Closure captures access cells via captures[] (not v->args), so they
 * correctly get the cell itself. */
#define CELL_UNWRAP_MAX 8
    uint32_t saved_ids[CELL_UNWRAP_MAX];
    uint8_t saved_regs[CELL_UNWRAP_MAX];
    uint8_t temp_regs[CELL_UNWRAP_MAX];
    int nsaved = 0;

    bool skip_unwrap = (v->op == XI_CLOSURE_NEW);
    for (uint16_t ai = 0; ai < v->nargs && nsaved < CELL_UNWRAP_MAX; ai++) {
        XiValue *arg = v->args[ai];
        if (!arg || arg->id >= ctx->reg_map_size)
            continue;
        if (skip_unwrap)
            continue;
        if (!ctx->cell_wrapped[arg->id] &&
            !(arg->var_id != 0xFF && ctx->cell_side_reg[arg->var_id] != NO_REG))
            continue;
        uint8_t cell_reg = reg_of(ctx, arg);
        if (ctx->status != XI_EMIT_OK)
            return;
        if (ctx->next_reg >= MAX_REGS - 1) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        uint8_t tmp = ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        emit_inst(ctx, CREATE_ABC(OP_CELL_GET, tmp, cell_reg, 0));
        saved_ids[nsaved] = arg->id;
        saved_regs[nsaved] = ctx->reg_map[arg->id];
        temp_regs[nsaved] = tmp;
        ctx->reg_map[arg->id] = tmp;
        nsaved++;
    }
#undef CELL_UNWRAP_MAX

    /* If the destination register is cell-wrapped and this is not a closure
     * (closures handle cell-set internally), redirect the handler to write
     * into a temp register and then CELL_SET into the real cell register.
     * This prevents overwriting cell pointers when a variable is re-assigned
     * after being cell-wrapped by a hoisted function's capture.
     *
     * Special case: if the cell hasn't been created yet (first definition),
     * emit the value normally then wrap with OP_CELL_NEW instead of CELL_SET. */
    uint8_t real_dst = dst;
    bool need_cell_set = false;
    bool need_cell_new = false;
    if (v->op != XI_CLOSURE_NEW && v->var_id != 0xFF && ctx->cell_side_reg[v->var_id] != NO_REG) {
        if (!ctx->cell_created[v->var_id]) {
            /* First definition: emit value to dst, then wrap with CELL_NEW */
            need_cell_new = true;
        } else {
            if (ctx->next_reg >= MAX_REGS - 1) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
                return;
            }
            uint8_t tmp = ctx->next_reg++;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
            ctx->reg_map[v->id] = tmp;
            dst = tmp;
            need_cell_set = true;
        }
    }

    XR_DCHECK(v->op >= 0 && v->op < XI_OP_COUNT, "emit_value: op out of range");
    XiEmitHandler handler = xi_emit_handlers[v->op];
    if (handler) {
        handler(ctx, v, dst);
    } else {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
    }

    if (need_cell_new && ctx->status == XI_EMIT_OK) {
        emit_inst(ctx, CREATE_ABC(OP_CELL_NEW, dst, 0, 0));
        ctx->cell_wrapped[v->id] = true;
        ctx->cell_created[v->var_id] = true;
    }

    if (need_cell_set && ctx->status == XI_EMIT_OK) {
        emit_inst(ctx, CREATE_ABC(OP_CELL_SET, real_dst, dst, 0));
        ctx->reg_map[v->id] = real_dst;
        free_reg(ctx, dst);
    }

    /* Restore original reg_map and free temp registers */
    for (int ri = 0; ri < nsaved; ri++) {
        ctx->reg_map[saved_ids[ri]] = saved_regs[ri];
        free_reg(ctx, temp_regs[ri]);
    }
}

/* ========== Public API ========== */

XR_FUNC XiEmitStatus xi_emit(XiFunc *f, struct XrayIsolate *isolate, struct XrProto **out_proto) {
    XR_DCHECK(f != NULL, "xi_emit: NULL func");
    XR_DCHECK(out_proto != NULL, "xi_emit: NULL out_proto");
    *out_proto = NULL;

    /* Run prerequisite analyses */
    uint32_t rpo_count = xi_compute_rpo(f);
    if (rpo_count == 0)
        return XI_EMIT_ERR_INTERNAL;

    /* Exception handler blocks are unreachable via normal CFG edges.
     * Scan for XI_TRY ops and assign RPO numbers to catch/finally targets
     * and all transitively reachable blocks (catch body, finally, merge). */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v->op != XI_TRY)
                continue;

            /* Determine the BFS seed: catch block if present, otherwise
             * the finally block.  For try-finally without catch, the
             * finally block is unreachable via normal CFG (throw sets
             * cur_block = NULL), so it needs RPO assignment here. */
            XiBlock *seed = (XiBlock *) v->aux; /* catch block or NULL */
            if (!seed && v->aux_int >= 0) {
                uint32_t fid = (uint32_t) v->aux_int;
                if (fid < f->nblocks)
                    seed = f->blocks[fid];
            }
            if (!seed)
                continue;

            XiBlock *queue[64];
            int qhead = 0, qtail = 0;
            if (seed->rpo == 0) {
                seed->rpo = ++rpo_count;
                queue[qtail++] = seed;
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

    /* Build RPO order array */
    XiBlock **rpo_order = (XiBlock **) xr_calloc(rpo_count + 1, sizeof(XiBlock *));
    if (!rpo_order)
        return XI_EMIT_ERR_INTERNAL;

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
    if (!ctx.proto) {
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }

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
    memset(ctx.var_reg, NO_REG, sizeof(ctx.var_reg));

    /* Allocate register map */
    ctx.reg_map_size = f->next_value_id;
    ctx.reg_map = (uint8_t *) xr_malloc(ctx.reg_map_size);
    if (!ctx.reg_map) {
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    memset(ctx.reg_map, NO_REG, ctx.reg_map_size);

    /* Allocate block PC map */
    ctx.block_pc_size = f->next_block_id;
    ctx.block_pc = (int *) xr_malloc(ctx.block_pc_size * sizeof(int));
    if (!ctx.block_pc) {
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    for (uint32_t i = 0; i < ctx.block_pc_size; i++)
        ctx.block_pc[i] = -1;

    /* Allocate last-use ordinal map for register recycling */
    ctx.last_use = (uint32_t *) xr_calloc(ctx.reg_map_size, sizeof(uint32_t));
    if (!ctx.last_use) {
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    compute_last_use(&ctx);

    /* Allocate cell-wrapped tracker for dedup of OP_CELL_NEW */
    ctx.cell_wrapped = (bool *) xr_calloc(ctx.reg_map_size, sizeof(bool));
    if (!ctx.cell_wrapped) {
        xr_free(ctx.last_use);
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }

    /* Initialize side cell register map for hoisted function captures */
    memset(ctx.cell_side_reg, NO_REG, sizeof(ctx.cell_side_reg));
    memset(ctx.cell_created, 0, sizeof(ctx.cell_created));

    /* Allocate per-value bytecode PC map for IC-guided JIT speculation */
    ctx.value_pc = (int *) xr_malloc(ctx.reg_map_size * sizeof(int));
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
    if (ctx.status != XI_EMIT_OK)
        goto cleanup;

    /* Pre-populate cell_side_reg for mutable captures so that blocks emitted
     * before the closure (e.g. loop condition blocks) correctly cell-deref.
     * Without this, a loop var captured by a closure in the body won't be
     * cell-dereferenced in the condition block which is emitted first in RPO. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v->op != XI_CLOSURE_NEW)
                continue;
            XiFunc *child = (XiFunc *) v->aux;
            if (!child)
                continue;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                XiCapture *cap = &child->captures[ci];
                if (!cap->needs_cell || cap->source != XI_CAPTURE_SRC_REG)
                    continue;
                if (!cap->value || cap->value->id >= ctx.reg_map_size)
                    continue;
                if (cap->value->var_id == 0xFF)
                    continue;
                uint8_t reg = ctx.reg_map[cap->value->id];
                if (reg == NO_REG)
                    continue;
                ctx.cell_side_reg[cap->value->var_id] = reg;
            }
        }
    }

    /* Emit blocks in RPO order */
    for (uint32_t r = 1; r <= rpo_count; r++) {
        XiBlock *blk = rpo_order[r];
        if (!blk)
            continue;
        XiBlock *next_blk = (r + 1 <= rpo_count) ? rpo_order[r + 1] : NULL;

        /* Before emitting block, emit phi moves from IF predecessors.
         * For PLAIN blocks, phi moves are emitted by the predecessor.
         * For IF predecessors, phi moves for the else path need to be
         * emitted at the else block's start. */
        emit_block(&ctx, blk, next_blk);
        if (ctx.status != XI_EMIT_OK)
            goto cleanup;
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
            if (f->entry->values[i]->op == XI_PARAM)
                pc++;
        }
        ctx.proto->numparams = pc;
    }
    ctx.proto->is_vararg = f->is_vararg;
    ctx.proto->entry_type = f->entry_type;
    ctx.proto->min_params = f->min_params;
    /* Set struct_area_size for VM per-frame struct allocation */
    ctx.proto->struct_area_size = (uint16_t) (ctx.struct_area_offset * 16);
    ctx.proto->test_attr = f->test_attr;
    ctx.proto->test_timeout = f->test_timeout;
    /* Propagate the declared return type so downstream pipelines (JIT
     * codegen RET, xm_jit_call result reconstruction, eligibility) can
     * tag values precisely instead of falling back to UNKNOWN → I64.
     * Skip VOID: anonymous / arrow functions inherit the void default
     * when no annotation exists, and forcing VOID onto the proto would
     * make the RET codegen emit XR_TAG_NULL even when the body actually
     * returns a string, closure, or other pointer. */
    if (!ctx.proto->return_type_info && f->return_type &&
        f->return_type->kind != XR_KIND_VOID)
        ctx.proto->return_type_info = f->return_type;
    /* Compile-time escape analysis is the authority on coroutine safety.
     * If compilation succeeds, all functions are safe to call via go. */
    ctx.proto->is_coro_safe = true;

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
        case XI_EMIT_OK:
            return "OK";
        case XI_EMIT_ERR_TOO_MANY_REGS:
            return "too many registers (>255)";
        case XI_EMIT_ERR_TOO_MANY_CONSTS:
            return "constant pool overflow";
        case XI_EMIT_ERR_UNSUPPORTED_OP:
            return "unsupported Xi IR operation";
        case XI_EMIT_ERR_INTERNAL:
            return "internal emitter error";
    }
    return "unknown error";
}
