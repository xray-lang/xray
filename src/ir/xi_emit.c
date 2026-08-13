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
#include "xi_emit_vm_gen.h"
#include "xi_opt.h"
#include "xi_stage.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/value/xffi_sig.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xbigint.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../toolchain/xcompiler_session.h"
#include "../frontend/analyzer/xa_effect_db.h"
#include "../shared/xr_native_type_core.h"
#include "../shared/xr_codegen_opaque_core.h"
#include "../shared/xr_truthy_core.h"

#include <math.h>

/* ========== Helpers ========== */

XR_FUNC void emit_error(EmitCtx *ctx, XiEmitStatus s) {
    if (ctx->status == XI_EMIT_OK)
        ctx->status = s;
}

XR_FUNC int current_pc(EmitCtx *ctx) {
    return PROTO_CODE_COUNT(ctx->proto);
}

XR_FUNC void emit_inst(EmitCtx *ctx, XrInstruction inst) {
    xr_instruction_unit_write(ctx->proto, inst, ctx->current_line);
}

XR_FUNC bool xi_emit_alloc_struct_area_slot(EmitCtx *ctx, const XrAggregateLayout *layout,
                                            uint16_t *slot_out) {
    if (!ctx || !layout || !slot_out) {
        if (ctx)
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return false;
    }

    uint32_t bytes_needed = xr_aggregate_layout_storage_size(layout);
    return xi_emit_alloc_struct_area_bytes(ctx, bytes_needed, slot_out);
}

XR_FUNC bool xi_emit_alloc_struct_area_bytes(EmitCtx *ctx, uint32_t bytes_needed,
                                             uint16_t *slot_out) {
    if (!ctx || bytes_needed == 0 || !slot_out) {
        if (ctx)
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return false;
    }

    uint32_t slot = ctx->struct_area_offset;
    if (bytes_needed > UINT32_MAX - 15u) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }
    uint32_t slots_needed = (bytes_needed + 15u) / 16u;
    if (slots_needed > UINT32_MAX - slot) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }
    uint32_t next = slot + slots_needed;
    if (slot > MAXARG_C || next > UINT32_MAX / 16u) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }

    ctx->struct_area_offset = next;
    *slot_out = (uint16_t) slot;
    return true;
}

XR_FUNC bool xi_emit_alloc_struct_area_bytes_wide(EmitCtx *ctx, uint32_t bytes_needed,
                                                  uint32_t *slot_out) {
    if (!ctx || bytes_needed == 0 || !slot_out) {
        if (ctx)
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return false;
    }
    uint32_t slot = ctx->struct_area_offset;
    if (bytes_needed > UINT32_MAX - 15u) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }
    uint32_t slots_needed = (bytes_needed + 15u) / 16u;
    if (slots_needed > UINT32_MAX - slot) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }
    uint32_t next = slot + slots_needed;
    if (slot > MAXARG_Bx || next > UINT32_MAX / 16u) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }
    ctx->struct_area_offset = next;
    *slot_out = slot;
    return true;
}

static void xi_emit_note_var_id(const XiValue *v, uint32_t *max_var_id, bool *has_var_id) {
    if (!v || !xi_var_id_is_valid(v->var_id))
        return;
    if (!*has_var_id || v->var_id > *max_var_id)
        *max_var_id = v->var_id;
    *has_var_id = true;
}

static uint32_t xi_emit_var_state_count(const XiFunc *f) {
    uint32_t max_var_id = 0;
    bool has_var_id = false;
    if (!f)
        return 0;

    for (uint16_t i = 0; i < f->nparams; i++)
        xi_emit_note_var_id(f->params[i], &max_var_id, &has_var_id);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control)
            xi_emit_note_var_id(blk->control, &max_var_id, &has_var_id);
        for (XiPhi *phi = blk->phis; phi; phi = phi->next)
            xi_emit_note_var_id(&phi->value, &max_var_id, &has_var_id);
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            xi_emit_note_var_id(v, &max_var_id, &has_var_id);
            if (!v || v->op != XI_CLOSURE_NEW || !v->aux)
                continue;
            XiFunc *child = (XiFunc *) v->aux;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                XiCapture *cap = &child->captures[ci];
                xi_emit_note_var_id(cap->value, &max_var_id, &has_var_id);
                if (ci < v->nargs)
                    xi_emit_note_var_id(v->args[ci], &max_var_id, &has_var_id);
            }
        }
    }

    return has_var_id ? max_var_id + 1u : 0u;
}

static void xi_emit_free_var_state(EmitCtx *ctx) {
    xr_free(ctx->var_reg);
    ctx->var_reg = NULL;
    ctx->var_state_count = 0;
}

static bool xi_emit_init_var_state(EmitCtx *ctx, XiFunc *f) {
    ctx->var_state_count = xi_emit_var_state_count(f);
    if (ctx->var_state_count == 0)
        return true;

    ctx->var_reg = (XiEmitReg *) xr_malloc((size_t) ctx->var_state_count * sizeof(*ctx->var_reg));
    if (!ctx->var_reg) {
        xi_emit_free_var_state(ctx);
        return false;
    }
    for (uint32_t i = 0; i < ctx->var_state_count; i++) {
        ctx->var_reg[i] = NO_REG;
    }
    return true;
}

/* Return a register to the free pool for reuse. */
XR_FUNC void free_reg(EmitCtx *ctx, XiEmitReg reg) {
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
XR_FUNC XiEmitReg reg_of(EmitCtx *ctx, const XiValue *v) {
    XR_DCHECK(v != NULL, "reg_of: NULL value");

    if (v->id >= ctx->reg_map_size) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return 0;
    }

    if (ctx->reg_map[v->id] == NO_REG) {
        /* Variable coalescing: reuse the pinned register for this var_id */
        if (xi_var_id_is_valid(v->var_id) && !xi_emit_var_id_in_state(ctx, v->var_id)) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return 0;
        }
        if (xi_emit_var_id_in_state(ctx, v->var_id) && ctx->var_reg[v->var_id] != NO_REG) {
            ctx->reg_map[v->id] = ctx->var_reg[v->var_id];
            return ctx->reg_map[v->id];
        }
        /* Try recycled register first */
        if (ctx->nfree > 0) {
            ctx->reg_map[v->id] = ctx->free_regs[--ctx->nfree];
        } else {
            if (ctx->next_reg >= MAX_REGS) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
                return 0;
            }
            ctx->reg_map[v->id] = (XiEmitReg) ctx->next_reg++;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
        }
        /* Record as the pinned register for this variable */
        if (xi_emit_var_id_in_state(ctx, v->var_id))
            ctx->var_reg[v->var_id] = ctx->reg_map[v->id];
    }
    return ctx->reg_map[v->id];
}

/* Like reg_of but never uses the free list — always allocates from next_reg.
 * Call instructions place args at dst+1..dst+nargs; a recycled low register
 * for dst could overlap with live source registers and cause clobber bugs.
 *
 * When var_id is set and var_reg is unset (first definition of the variable
 * comes from a call-like op), record var_reg so that subsequent definitions
 * and exception-path reads find a consistent register. */
XR_FUNC XiEmitReg alloc_reg_fresh(EmitCtx *ctx, const XiValue *v) {
    XR_DCHECK(v != NULL, "alloc_reg_fresh: NULL value");
    if (v->id >= ctx->reg_map_size) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return 0;
    }
    if (ctx->reg_map[v->id] == NO_REG) {
        if (ctx->next_reg >= MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return 0;
        }
        ctx->reg_map[v->id] = (XiEmitReg) ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        /* First definition via call-like op: pin var_reg so later defs
         * and exception-path reads coalesce to this register. */
        if (xi_var_id_is_valid(v->var_id) && !xi_emit_var_id_in_state(ctx, v->var_id)) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return 0;
        }
        if (xi_emit_var_id_in_state(ctx, v->var_id) && ctx->var_reg[v->var_id] == NO_REG)
            ctx->var_reg[v->var_id] = ctx->reg_map[v->id];
    }
    return ctx->reg_map[v->id];
}

/* Release registers of input args whose last use is at the current ordinal.
 * Called AFTER emitting an instruction that reads these args.
 * Coalesced registers (var_id != XI_NO_VAR_ID) are never freed — they must remain
 * pinned so all SSA definitions of the variable share one VM register. */
XR_FUNC void try_free_args(EmitCtx *ctx, const XiValue *v) {
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg->id >= ctx->reg_map_size)
            continue;
        if (xi_var_id_is_valid(arg->var_id))
            continue; /* pinned by coalescing */
        /* Free register if this is the last use of arg */
        if (ctx->last_use[arg->id] == ctx->current_ordinal) {
            XiEmitReg r = ctx->reg_map[arg->id];
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

/* Add a pending OP_TRY patch (catch absolute PC target). */
XR_FUNC void add_try_patch(EmitCtx *ctx, int pc, uint32_t catch_bid) {
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
    ctx->ntry_patch++;
}

/* Add constant to pool, return index. */
XR_FUNC int add_const_int(EmitCtx *ctx, int64_t val) {
    XrValue xv = xr_make_int_val(val, XR_TAG_I64);
    int idx = xr_instruction_unit_add_constant(ctx->proto, xv);
    if (idx < 0 || (uint64_t) idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

XR_FUNC int add_const_float(EmitCtx *ctx, double val) {
    XrValue xv = xr_make_float_val(val, XR_TAG_F64);
    int idx = xr_instruction_unit_add_constant(ctx->proto, xv);
    if (idx < 0 || (uint64_t) idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

XR_FUNC int add_const_char(EmitCtx *ctx, uint32_t cp) {
    XrValue xv = xr_rune(cp);
    int idx = xr_instruction_unit_add_constant(ctx->proto, xv);
    if (idx < 0 || (uint64_t) idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

XR_FUNC int add_const_string_n(EmitCtx *ctx, const char *str, size_t len) {
    XrValue xv;
    if (ctx->isolate && (str || len == 0)) {
        XrString *xs = xr_string_intern_permanent(ctx->isolate, str ? str : "", len);
        if (!xs) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return -1;
        }
        xv = xr_string_value(xs);
    } else {
        /* No isolate: raw C string pointers are not valid heap objects,
         * so xr_make_ptr_val would read garbage object header bytes.
         * Use null placeholder — callers without isolate only check
         * instruction sequences, not constant pool values. */
        xv = xr_null();
    }
    int idx = xr_instruction_unit_add_constant(ctx->proto, xv);
    if (idx < 0 || (uint64_t) idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

XR_FUNC int add_const_string(EmitCtx *ctx, const char *str) {
    return add_const_string_n(ctx, str, str ? strlen(str) : 0);
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
    if (local < 0) {
        /* OOM growing the symbol table. Without this check the -1 would
         * be truncated by callers' narrow casts and silently bind the
         * wrong symbol. */
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return -1;
    }
    if (local > (int) MAXARG_B) {
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

static void emit_i64_const_reg(EmitCtx *ctx, XiEmitReg dst, int64_t val) {
    if (val >= LOADI_MIN && val <= LOADI_MAX) {
        emit_inst(ctx, CREATE_AsBx(OP_LOADI, dst, (int) val));
    } else {
        int ki = add_const_int(ctx, val);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
    }
}

void xi_emit_i64_const_reg(EmitCtx *ctx, XiEmitReg dst, int64_t value) {
    emit_i64_const_reg(ctx, dst, value);
}

static void emit_const(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    struct XrType *ty = v->type;
    if (!ty) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    switch (ty->kind) {
        case XR_KIND_INT:
        case XR_KIND_POINTER: {
            emit_i64_const_reg(ctx, dst, v->aux_int);
            break;
        }
        case XR_KIND_FLOAT: {
            double fval;
            memcpy(&fval, &v->aux_int, sizeof(double));
            /* C99 6.3.1.4: casting a double to int when the value falls
             * outside the int range is UB. Pre-check with double-vs-double
             * comparisons (NaN compares false in both directions, so it
             * naturally falls through to the constant-pool branch) and
             * only cast once we know the value fits the LOADI immediate. */
            if (fval >= (double) LOADI_MIN && fval <= (double) LOADI_MAX) {
                int sv = (int) fval;
                /* Reconstructing the float from an int immediate must reproduce
                 * the exact value. `(double) sv == fval` treats -0.0 and +0.0 as
                 * equal even though their bit patterns differ, so route -0.0
                 * through the constant pool to preserve its sign. */
                if ((double) sv == fval && (sv != 0 || !signbit(fval))) {
                    emit_inst(ctx, CREATE_AsBx(OP_LOADF, dst, sv));
                    break;
                }
            }
            int ki = add_const_float(ctx, fval);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            break;
        }
        case XR_KIND_BOOL:
            if (v->aux_int)
                emit_inst(ctx, CREATE_ABC(OP_LOADTRUE, dst, 0, 0));
            else
                emit_inst(ctx, CREATE_ABC(OP_LOADFALSE, dst, 0, 0));
            break;
        case XR_KIND_RUNE: {
            int ki = add_const_char(ctx, (uint32_t) v->aux_int);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
            break;
        }
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
            /* Enum metadata views/descriptors are compiler intrinsics with an
             * I64 representation. Their nominal generic type keeps values
             * such as EnumVariant<E> distinct in the analyzer, but bytecode
             * must load the packed scalar rather than a null instance. */
            if (xr_type_is_enum_metadata(ty)) {
                emit_i64_const_reg(ctx, dst, v->aux_int);
                break;
            }
            /* BigInt: aux holds decimal digit string, create XrBigInt object */
            if (xr_type_is_builtin_named_class(ty, "BigInt") && v->aux) {
                const char *digits = (const char *) v->aux;
                XrBigInt *bi = xr_bigint_from_string_on_fixed_heap(
                    xr_isolate_get_fixed_heap(ctx->isolate), digits);
                if (!bi) {
                    emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                    return;
                }
                XrayCoreClasses *core = xr_isolate_get_core_classes(ctx->isolate);
                if (core)
                    bi->klass = core->bigintClass;
                XrValue xv = XR_FROM_PTR(bi);
                int ki = xr_instruction_unit_add_constant(ctx->proto, xv);
                if (ki < 0 || (uint64_t) ki > MAXARG_Bx) {
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
            void *ptr = v->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE && v->aux
                            ? ((XiEnumData *) v->aux)->runtime_type
                            : v->aux;
            if (ptr) {
                XrValue xv = XR_FROM_PTR(ptr);
                int ki = xr_instruction_unit_add_constant(ctx->proto, xv);
                if (ki < 0 || (uint64_t) ki > MAXARG_Bx) {
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

static void xi_emit_target_layout_query(EmitCtx *ctx, XiValue *v, XiEmitReg dst,
                                        XrTargetLayoutQueryKind kind) {
    XrTargetLayoutQueryResult result = XR_TARGET_LAYOUT_QUERY_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_TARGET_LAYOUT_QUERY_HI,
        XR_SEM_OWNER_ID_SHARED_TARGET_LAYOUT_QUERY_LO, XR_SEM_CONSUMER_VM,
        xr_target_layout_query_core(kind, ctx ? ctx->target_data_layout : NULL,
                                    (uint8_t) (v ? v->aux_int : 0)));
    if (result.status != XR_TARGET_LAYOUT_QUERY_OK) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    emit_i64_const_reg(ctx, dst, (int64_t) result.value);
}

static void xi_emit_target_sizeof(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    xi_emit_target_layout_query(ctx, v, dst, XR_TARGET_LAYOUT_QUERY_SIZE);
}

static void xi_emit_target_alignof(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    xi_emit_target_layout_query(ctx, v, dst, XR_TARGET_LAYOUT_QUERY_ALIGN);
}

static void emit_param(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) ctx;
    (void) v;
    (void) dst;
    /* Params already in registers; no-op. */
}

static XrAggregateLayout *emit_value_struct_layout(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return NULL;
    if (!type->instance.class_ref)
        return NULL;
    return type->instance.class_ref->struct_layout;
}

static void emit_copy(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!v || v->nargs < 1 || !v->args[0]) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    bool has_enum_metadata = v->enum_metadata_owner != NULL || v->enum_metadata_field != 0 ||
                             v->enum_metadata_kind != 0;
    XrCopyPlan plan = XR_COPY_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_COPY_HI, XR_SEM_OWNER_ID_SHARED_COPY_LO,
        XR_SEM_CONSUMER_VM, v->aux_int, has_enum_metadata);
    if (!xr_copy_plan_is_exact_core(plan)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    /* Only lowering-marked value copies need independent storage. Optimizer
     * copies are identity aliases, including for value-struct typed values. */
    if (!plan.requires_independent_value) {
        if (dst != src)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
        return;
    }

    XrType *src_type = v->type ? v->type : v->args[0]->type;
    XrAggregateLayout *layout = emit_value_struct_layout(src_type);
    XiValue *origin = xi_emit_trace_struct_origin(v->args[0]);
    if (layout && origin && origin->op == XI_AGG_NEW && XI_EMIT_STRUCT_IS_PROMOTED(origin)) {
        uint16_t slot = 0;
        if (!xi_emit_alloc_struct_area_slot(ctx, layout, &slot))
            return;
        emit_inst(ctx, CREATE_ABC(OP_AGG_COPY, dst, src, slot));
        return;
    }
    emit_inst(ctx, CREATE_ABC(OP_COPY, dst, src, 0));
}

/* VM execution has no native optimizer to constrain.  Keep the canonical Xi
 * nodes for explain/verification, then realize their observable identity/no-op
 * semantics in bytecode. */
static void emit_codegen_opaque(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!v || v->nargs != 1 || !v->args[0]) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XrCodegenOpaquePlan plan = XR_CODEGEN_OPAQUE_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_HI,
        XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_LO, XR_SEM_CONSUMER_VM,
        XR_CODEGEN_OPAQUE_VALUE);
    if (!xr_codegen_opaque_plan_is_exact_core(plan)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status == XI_EMIT_OK && dst != src)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
}

static void emit_codegen_compiler_fence(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) v;
    emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
}

/* Conditional select: dst = cond ? true_val : false_val.
 * OP_TEST skips the next instruction when truthiness differs from B.  The
 * alias cases are important for coalesced source variables: default-parameter
 * guards often write the SELECT result back into the original parameter slot. */
static void emit_select(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    (void) xr_truthy_core_eval(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                               XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                               XR_SEM_CONSUMER_VM, XR_TRUTHY_CORE_BOOL, 1, 0.0, 0);
    XiEmitReg cond_r = reg_of(ctx, v->args[0]);
    XiEmitReg true_r = reg_of(ctx, v->args[1]);
    XiEmitReg false_r = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;

    if (true_r == false_r) {
        if (dst != true_r)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, true_r, 0));
        return;
    }

    if (dst == true_r) {
        emit_inst(ctx, CREATE_ABC(OP_TEST, cond_r, 0, 0));
        if (dst != false_r)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, false_r, 0));
        return;
    }

    if (dst == false_r) {
        emit_inst(ctx, CREATE_ABC(OP_TEST, cond_r, 1, 0));
        if (dst != true_r)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, true_r, 0));
        return;
    }

    bool copied_cond = false;
    XiEmitReg test_r = cond_r;
    if (dst == cond_r) {
        if (ctx->next_reg >= MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        test_r = (XiEmitReg) ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        emit_inst(ctx, CREATE_ABC(OP_MOVE, test_r, cond_r, 0));
        copied_cond = true;
    }

    if (dst != false_r)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, false_r, 0));
    emit_inst(ctx, CREATE_ABC(OP_TEST, test_r, 1, 0));
    if (dst != true_r)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, true_r, 0));
    if (copied_cond)
        free_reg(ctx, test_r);
}

/* ========== Dispatch Table ========== */

const XiEmitHandler xi_emit_handlers[XI_OP_COUNT] = {
#define XI_VM_HANDLER_ENTRY(op, handler) [XI_##op] = handler,
    XI_EMIT_VM_LOWERING_HANDLERS(XI_VM_HANDLER_ENTRY)
#undef XI_VM_HANDLER_ENTRY
        [XI_PAR_FOR] = xi_emit_par_for,
    [XI_PAR_MAP] = xi_emit_par_map,
    [XI_PAR_REDUCE] = xi_emit_par_reduce,
    [XI_PHI] = NULL, /* handled separately by emit_phi_moves */
};

/* ========== Instruction Selection ========== */

XR_FUNC void emit_value(EmitCtx *ctx, XiValue *v) {
    if (ctx->status != XI_EMIT_OK)
        return;

    /* Skip comparison that was absorbed into the block terminator */
    if (v == ctx->fused_cmp)
        return;

    bool fresh_dst = xi_emit_vm_requires_fresh_dst(v->op) || xi_copy_is_cleanup_return(v);
    XiEmitReg dst = fresh_dst ? alloc_reg_fresh(ctx, v) : reg_of(ctx, v);
    if (ctx->status != XI_EMIT_OK)
        return;

    XR_DCHECK(v->op >= 0 && v->op < XI_OP_COUNT, "emit_value: op out of range");
    XiEmitHandler handler = xi_emit_handlers[v->op];
    if (handler) {
        handler(ctx, v, dst);
    } else {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
    }

    /* Fresh-dst handlers reserve a VM register window around the result.
     * Copy the value back to a coalesced variable register so exception
     * edges and merge-point phi moves see the expected register. */
    if (fresh_dst && ctx->status == XI_EMIT_OK && xi_emit_var_id_in_state(ctx, v->var_id)) {
        XiEmitReg vr = ctx->var_reg[v->var_id];
        XiEmitReg fresh_reg = ctx->reg_map[v->id];
        if (vr != NO_REG && vr != fresh_reg) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, vr, fresh_reg, 0));
        }
    }
}

/* ========== Public API ========== */

XR_FUNC XiEmitStatus xi_emit(XiFunc *f, struct XrVMRuntime *isolate, struct XrProto **out_proto) {
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
    XrCompilerSession *compiler_session = xr_compiler_session_current_for_isolate(isolate);
    ctx.target_data_layout = compiler_session
                                 ? xr_compiler_session_target_data_layout(compiler_session)
                                 : xr_target_data_layout_host();
    if (!ctx.target_data_layout) {
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    ctx.proto = xr_instruction_unit_new();
    if (!ctx.proto) {
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    if (isolate && f->name && f->name[0])
        ctx.proto->name = xr_string_intern_permanent(isolate, f->name, strlen(f->name));
    if (f->source_file && f->source_file[0])
        ctx.proto->source_file = xr_strdup(f->source_file);

    /* Top-level variables are stored in the name-keyed globals dict
     * via OP_GETGLOBAL / OP_SETGLOBAL.  The legacy shared array is still
     * needed for module export metadata, but allocation belongs to the VM
     * load/execute boundary.  Emit only records the proto tree's local slot
     * count so source compile does not mutate VM execution state. */
    ctx.proto->shared_count = f->nshared;

    ctx.rpo_order = rpo_order;
    ctx.rpo_count = rpo_count;

    /* Allocate register map */
    ctx.reg_map_size = f->next_value_id;
    ctx.reg_map = (XiEmitReg *) xr_malloc(ctx.reg_map_size * sizeof(*ctx.reg_map));
    if (!ctx.reg_map) {
        xr_instruction_unit_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    memset(ctx.reg_map, 0xFF, ctx.reg_map_size * sizeof(*ctx.reg_map));

    /* Allocate block PC map */
    ctx.block_pc_size = f->next_block_id;
    ctx.block_pc = (int *) xr_malloc(ctx.block_pc_size * sizeof(int));
    if (!ctx.block_pc) {
        xr_free(ctx.reg_map);
        xr_instruction_unit_free(ctx.proto);
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
        xr_instruction_unit_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    compute_last_use(&ctx);

    if (!xi_emit_init_var_state(&ctx, f)) {
        xr_free(ctx.last_use);
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_instruction_unit_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }

    alloc_registers(&ctx);
    if (ctx.status != XI_EMIT_OK)
        goto cleanup;

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
    ctx.proto->call_place_param_bitmap = 0;
    for (uint16_t pi = 0; pi < f->nparams && pi < 64; pi++) {
        XiValue *param = f->params ? f->params[pi] : NULL;
        XrParamMode mode = xi_func_param_passing_mode(f, pi);
        bool is_place = (pi == 0 && f->receiver_call_place) || mode == XR_PARAM_REF ||
                        xi_value_is_read_place_param(param);
        if (is_place)
            ctx.proto->call_place_param_bitmap |= UINT64_C(1) << pi;
    }
    /* Set struct_area_size for VM per-frame struct allocation */
    ctx.proto->struct_area_size = ctx.struct_area_offset * 16u;
    ctx.proto->test_attr = f->test_attr;
    ctx.proto->test_timeout = f->test_timeout;
    ctx.proto->is_extern = f->is_extern;
    /* FFI: build the self-contained extern signature on the proto so the VM's
     * libffi invoker works without the Xi IR (which is not serialized into an
     * embedded bytecode binary). The AOT backend ignores this and emits direct
     * C calls. */
    if (f->is_extern && !ctx.proto->ffi_sig) {
        const char *sym = f->extern_symbol ? f->extern_symbol : f->name;
        XrFFISig *sig = xr_ffi_sig_new(sym, f->extern_dylib, (uint8_t) f->nparams);
        if (sig) {
            for (uint16_t pi = 0; pi < f->nparams; pi++) {
                const struct XrType *pt = (f->params && f->params[pi]) ? f->params[pi]->type : NULL;
                sig->params[pi] = xr_ffi_type_from_xrtype(pt, false);
                if (pt && pt->kind == XR_KIND_FUNCTION && pt->function.is_c_abi)
                    xr_ffi_sig_set_param_callback(sig, (uint8_t) pi, pt);
            }
            sig->ret = xr_ffi_type_from_xrtype(f->return_type, true);
            ctx.proto->ffi_sig = sig;
        }
    }
    /* Propagate the declared return type so downstream AOT codegen (RET
     * tagging) can tag values precisely instead of falling back to
     * UNKNOWN → I64.
     * Skip VOID: anonymous / arrow functions inherit the void default
     * when no annotation exists, and forcing VOID onto the proto would
     * make the RET codegen emit XR_TAG_NULL even when the body actually
     * returns a string, closure, or other pointer. */
    if (!ctx.proto->return_type_info && f->return_type && f->return_type->kind != XR_KIND_UNIT)
        ctx.proto->return_type_info = f->return_type;
    /* Compile-time escape analysis is the authority on coroutine safety.
     * If compilation succeeds, all functions are safe to call via go. */
    ctx.proto->is_coro_safe = true;
    ctx.proto->may_scheduler_suspend = (f->semantic_effects & XA_SEM_EFFECT_SCHED_SUSPEND) != 0;
    ctx.proto->may_task_spawn = (f->semantic_effects & XA_SEM_EFFECT_TASK_SPAWN) != 0;

cleanup:;
    XiEmitStatus result = ctx.status;
    if (result == XI_EMIT_OK) {
        *out_proto = ctx.proto;
    } else {
        xr_instruction_unit_free(ctx.proto);
    }
    xi_emit_free_var_state(&ctx);
    xr_free(ctx.last_use);
    xr_free(ctx.reg_map);
    xr_free(ctx.block_pc);
    xr_free(ctx.patches);
    xr_free(ctx.try_patches);
    xr_free(rpo_order);
    return result;
}

static bool xi_emit_ir_tree_can_attach(const XrProto *proto, const XiFunc *ir) {
    if (!proto || !ir || proto->xi_func || ir->stage < XI_STAGE_REPPED ||
        PROTO_PROTO_COUNT(proto) != ir->nchildren)
        return false;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        XrProto *child_proto = PROTO_PROTO(proto, i);
        XiFunc *child_ir = ir->children[i];
        if (!xi_emit_ir_tree_can_attach(child_proto, child_ir))
            return false;
    }
    return true;
}

static void xi_emit_ir_tree_commit(XrProto *proto, XiFunc *ir) {
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        XrProto *child_proto = PROTO_PROTO(proto, i);
        XiFunc *child_ir = ir->children[i];
        xi_emit_ir_tree_commit(child_proto, child_ir);
        ir->children[i] = NULL;
    }
    proto->xi_func = ir;
}

XR_FUNC bool xi_emit_attach_ir(struct XrProto *proto, XiFunc *ir) {
    XR_DCHECK(proto != NULL, "xi_emit_attach_ir: NULL proto");
    XR_DCHECK(proto->xi_func == NULL, "xi_emit_attach_ir: proto already has xi_func");

    if (ir && ir->stage == XI_STAGE_SEMANTIC_PLANNED) {
        char error[512] = {0};
        XiSemanticPlannedProgram *semantic =
            xi_stage_adopt_semantic_planned(ir, error, sizeof(error));
        if (!semantic)
            return false;
        XiRepPolicy policy = xi_rep_policy_tagged_boundary();
        xi_opt_select_rep_with_policy(ir, &policy);
        xi_opt_box_elim(ir);
        XiReppedProgram *repped = xi_program_select_reps(semantic, error, sizeof(error));
        if (!repped) {
            xi_semantic_planned_program_release(semantic);
            return false;
        }
        ir = xi_repped_program_release(repped);
    }
    if (!xi_emit_ir_tree_can_attach(proto, ir))
        return false;
    xi_emit_ir_tree_commit(proto, ir);
    return true;
}

XR_FUNC const char *xi_emit_status_str(XiEmitStatus s) {
    switch (s) {
        case XI_EMIT_OK:
            return "OK";
        case XI_EMIT_ERR_TOO_MANY_REGS:
            return "too many registers (>65535 encoded slots, with one sentinel reserved)";
        case XI_EMIT_ERR_TOO_MANY_CONSTS:
            return "constant pool overflow";
        case XI_EMIT_ERR_UNSUPPORTED_OP:
            return "unsupported Xi IR operation";
        case XI_EMIT_ERR_INTERNAL:
            return "internal emitter error";
    }
    return "unknown error";
}
