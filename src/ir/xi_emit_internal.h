/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_internal.h - Shared internals for the Xi IR bytecode emitter
 *
 * Declares EmitCtx and utility functions used by all xi_emit_*.c sub-files.
 * Not part of the public API — only included by emitter implementation files.
 */

#ifndef XI_EMIT_INTERNAL_H
#define XI_EMIT_INTERNAL_H

#include "xi_emit.h"
#include "xi.h"
#include "xi_module.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/value/xtype.h"
#include <string.h>

struct XrStructLayout;

/* ========== Constants ========== */

typedef uint16_t XiEmitReg;

/* 0xffff is reserved as the emitter's "no register" sentinel. */
#define MAX_REGS ((uint32_t) UINT16_MAX)
#define NO_REG ((XiEmitReg) UINT16_MAX)

#define XI_EMIT_STRUCT_PROMOTED_BIT ((int64_t) 1 << 32)
#define XI_EMIT_STRUCT_IS_PROMOTED(v) (((v)->aux_int & XI_EMIT_STRUCT_PROMOTED_BIT) != 0)

static inline XiValue *xi_emit_trace_struct_origin(XiValue *v) {
    while (v && (xi_copy_is_identity_alias(v) || v->op == XI_MOVE) && v->nargs >= 1)
        v = v->args[0];
    return v;
}

static inline bool xi_emit_type_is_unsigned_int(const XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->is_nullable)
        return false;
    switch (type->native_width) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
            return true;
        default:
            return false;
    }
}

static inline bool xi_emit_type_is_int_like(const XrType *type) {
    return type && type->kind == XR_KIND_INT && !type->is_nullable;
}

static inline int xi_emit_tostring_hint_for_type(const XrType *type) {
    if (!type || type->is_nullable)
        return 0;
    if (xi_emit_type_is_unsigned_int(type))
        return 3;
    if (type->kind == XR_KIND_INT)
        return 1;
    if (type->kind == XR_KIND_FLOAT)
        return 2;
    return 0;
}

static inline bool xi_emit_compare_uses_unsigned(const XiValue *v) {
    if (!v || v->nargs < 2)
        return false;
    if (v->op != XI_LT && v->op != XI_LE && v->op != XI_GT && v->op != XI_GE)
        return false;
    const XrType *left = v->args[0] ? v->args[0]->type : NULL;
    const XrType *right = v->args[1] ? v->args[1]->type : NULL;
    return xi_emit_type_is_int_like(left) && xi_emit_type_is_int_like(right) &&
           (xi_emit_type_is_unsigned_int(left) || xi_emit_type_is_unsigned_int(right));
}

/* ========== Emit Context ========== */

typedef struct {
    XiFunc *func;
    XrProto *proto;
    struct XrayIsolate *isolate; /* for string interning; may be NULL */
    XiEmitStatus status;

    /* Register allocation: value_id -> register number */
    XiEmitReg *reg_map; /* [next_value_id] */
    uint32_t reg_map_size;
    uint32_t next_reg; /* next free register */
    uint32_t max_reg;  /* high-water mark */

    /* Free register stack for register recycling */
    XiEmitReg free_regs[MAX_REGS];
    uint16_t nfree; /* count of free registers on the stack */

    /* Liveness: per-value last-use tracking (value_id -> last-use ordinal) */
    uint32_t *last_use;       /* [next_value_id], 0 = unused/dead */
    uint32_t current_ordinal; /* monotonic instruction counter */

    /* Line number tracking for debug info */
    int current_line; /* line of the value being emitted */

    /* Struct-area slot allocator: tracks byte offset for OP_NEW_STRUCT.
     * Each struct occupies ceil16(xr_struct_layout_storage_size(layout)) bytes.
     * Proto->struct_area_size is set to this at end of emit. */
    uint16_t struct_area_offset; /* running byte offset (in 16-byte units) */

    /* Block linearization */
    XiBlock **rpo_order; /* blocks in RPO order */
    uint32_t rpo_count;

    /* Jump patching: block_id -> start PC */
    int *block_pc; /* [next_block_id], -1 = not yet emitted */
    uint32_t block_pc_size;

    /* Pending jump patches: instructions that need target PCs */
    struct {
        int pc;              /* instruction PC to patch */
        uint32_t target_bid; /* target block ID */
    } *patches;
    uint32_t npatch;
    uint32_t patch_cap;

    /* OP_TRY patches: absolute target PC patching (catch block) */
    struct {
        int pc;              /* OP_TRY instruction PC */
        uint32_t target_bid; /* catch block ID */
    } *try_patches;
    uint32_t ntry_patch;
    uint32_t try_patch_cap;

    /* Track which value IDs have been wrapped in a cell (OP_CELL_NEW).
     * Prevents double-wrapping when multiple closures capture the same
     * mutable variable. */
    bool *cell_wrapped; /* [next_value_id] */

    /* Comparison-branch fusion: if the block control is a comparison with
     * no other consumers, skip emitting OP_CMP_* and instead emit the
     * branch-form opcode (OP_LT/LE/EQ) directly in the terminator. */
    XiValue *fused_cmp;

    /* Per-var_id state capacity, sized to max(var_id)+1 for this function. */
    uint32_t var_state_count;

    /* Side cell register map: maps var_id → cell register for hoisted
     * function captures.  The cell is allocated in a separate register
     * so the original register remains usable for direct parent calls.
     * NO_REG (0xffff) means no cell allocated for this variable. */
    XiEmitReg *cell_side_reg; /* [var_state_count], var_id → cell register */

    /* Tracks whether OP_CELL_NEW has been emitted for a given var_id.
     * First write to a cell_side_reg variable emits CELL_NEW; subsequent
     * writes emit CELL_SET. */
    bool *cell_created; /* [var_state_count], var_id → true if CELL_NEW emitted */

    /* Variable-based register coalescing: all SSA definitions of the same
     * source variable share one VM register.  This is required for correct
     * exception semantics — the VM's OP_THROW bypasses SSA phi resolution,
     * so catch-block modifications must write to the same register that
     * post-try-catch code reads from. */
    XiEmitReg *var_reg; /* [var_state_count], var_id -> pinned register (NO_REG = unassigned) */
} EmitCtx;

static inline bool xi_emit_var_id_in_state(const EmitCtx *ctx, XiVarId var_id) {
    return ctx && xi_var_id_is_valid(var_id) && var_id < ctx->var_state_count;
}

static inline bool xi_emit_var_has_side_cell(const EmitCtx *ctx, XiVarId var_id) {
    return xi_emit_var_id_in_state(ctx, var_id) && ctx->cell_side_reg[var_id] != NO_REG;
}

/* ========== Shared Utility Functions ========== */

XR_FUNC void emit_error(EmitCtx *ctx, XiEmitStatus s);
XR_FUNC int current_pc(EmitCtx *ctx);
XR_FUNC void emit_inst(EmitCtx *ctx, XrInstruction inst);
XR_FUNC bool xi_emit_alloc_struct_area_slot(EmitCtx *ctx, const struct XrStructLayout *layout,
                                            uint16_t *slot_out);
XR_FUNC void free_reg(EmitCtx *ctx, XiEmitReg reg);
XR_FUNC XiEmitReg reg_of(EmitCtx *ctx, const XiValue *v);
XR_FUNC XiEmitReg reg_of_cell_deref(EmitCtx *ctx, const XiValue *v);
XR_FUNC XiEmitReg alloc_reg_fresh(EmitCtx *ctx, const XiValue *v);
XR_FUNC void try_free_args(EmitCtx *ctx, const XiValue *v);
XR_FUNC int add_const_int(EmitCtx *ctx, int64_t val);
XR_FUNC int add_const_float(EmitCtx *ctx, double val);
XR_FUNC int add_const_string(EmitCtx *ctx, const char *str);
XR_FUNC int add_symbol(EmitCtx *ctx, const char *name);

static inline bool xi_emit_u16_arg(EmitCtx *ctx, int64_t value, XiEmitStatus error, uint16_t *out) {
    if (value < 0 || (uint64_t) value > MAXARG_A) {
        emit_error(ctx, error);
        return false;
    }
    *out = (uint16_t) value;
    return true;
}

static inline bool xi_emit_const_index_to_c(EmitCtx *ctx, int ki, uint16_t *out) {
    return xi_emit_u16_arg(ctx, ki, XI_EMIT_ERR_TOO_MANY_CONSTS, out);
}

static inline bool xi_emit_symbol_index_to_arg(EmitCtx *ctx, int si, uint16_t *out) {
    return xi_emit_u16_arg(ctx, si, XI_EMIT_ERR_TOO_MANY_CONSTS, out);
}

static inline bool xi_emit_index_to_arg(EmitCtx *ctx, int64_t idx, XiEmitStatus error,
                                        uint16_t *out) {
    return xi_emit_u16_arg(ctx, idx, error, out);
}

XR_FUNC void xi_emit_add_patch(EmitCtx *ctx, int pc, uint32_t target_bid);
XR_FUNC void add_try_patch(EmitCtx *ctx, int pc, uint32_t catch_bid);

/* ========== Functions from xi_emit_reg.c ========== */
XR_FUNC void compute_last_use(EmitCtx *ctx);
XR_FUNC void alloc_registers(EmitCtx *ctx);

/* ========== Functions from xi_emit_cf.c ========== */
XR_FUNC void emit_phi_moves(EmitCtx *ctx, XiBlock *pred, XiBlock *succ);
XR_FUNC void emit_block(EmitCtx *ctx, XiBlock *blk, XiBlock *next_blk);
XR_FUNC void patch_jumps(EmitCtx *ctx);

/* emit_value is defined in xi_emit.c (driver) and called by emit_block */
XR_FUNC void emit_value(EmitCtx *ctx, XiValue *v);

/* ========== Generated Bytes lowering drivers ========== */
XR_FUNC void xi_emit_bytes_load_u32_le(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_bytes_load_u64_le(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_bytes_copy_within(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_bytes_copy_from(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_bytes_repeat_from(EmitCtx *ctx, XiValue *v, XiEmitReg dst);

/* ========== Generated FFI raw-pointer lowering drivers ========== */
XR_FUNC void xi_emit_ptr_load(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_ptr_store(EmitCtx *ctx, XiValue *v, XiEmitReg dst);

/* ========== Emit Handler Type ========== */

/* Signature for opcode-specific emission handlers.
 * ctx:  emitter context
 * v:    the Xi IR value to emit
 * dst:  pre-allocated destination register */
typedef void (*XiEmitHandler)(EmitCtx *ctx, XiValue *v, XiEmitReg dst);

/* Handler table: indexed by XiOp, NULL entries fall back to error. */
extern const XiEmitHandler xi_emit_handlers[XI_OP_COUNT];

/* ========== Handler Declarations (xi_emit_arith.c) ========== */
XR_FUNC void xi_emit_arith(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_neg(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_not(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_bnot(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_cmp(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_convert(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_box(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_unbox(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_narrow(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_widen(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_isnull(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_is(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_as(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_typeof(EmitCtx *ctx, XiValue *v, XiEmitReg dst);

/* ========== Handler Declarations (xi_emit_call.c) ========== */
XR_FUNC void xi_emit_call(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_call_method(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_call_method_direct(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_tail_call(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_call_builtin(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_str_concat(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_print(EmitCtx *ctx, XiValue *v, XiEmitReg dst);

/* ========== Handler Declarations (xi_emit_object.c) ========== */
XR_FUNC void xi_emit_load_field(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_store_field(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_struct_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_struct_get(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_struct_set(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_index_get(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_index_set(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_array_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_tuple_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_tuple_get(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_map_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_set_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_json_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_json_init_f(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_json_get_f(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_json_set_f(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_json_decode(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_range(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_slice(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_closure_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_load_upval(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_store_upval(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_get_shared(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_set_shared(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_get_global(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_set_global(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_get_builtin(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_iter(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_class_create(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_import_ref(EmitCtx *ctx, XiValue *v, XiEmitReg dst);

/* ========== Handler Declarations (xi_emit_eh.c) ========== */
XR_FUNC void xi_emit_throw(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_retain(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_release(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_move(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_err_set(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_err_return(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_err_check(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_err_catch(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_try(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_catch(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_end_try(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_defer(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_defer_mark(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_defer_run_to(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_go(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_await(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_yield(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_send(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_recv(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_recv_status(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_try_send(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_try_recv(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_is_closed(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_time_after(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_chan_timer_dispose(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_select_block(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_coro_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_scope_enter(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_scope_exit(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_assert(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_assert_eq(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_assert_ne(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_assert_throws(EmitCtx *ctx, XiValue *v, XiEmitReg dst);
XR_FUNC void xi_emit_regex_compile(EmitCtx *ctx, XiValue *v, XiEmitReg dst);

#endif  // XI_EMIT_INTERNAL_H
