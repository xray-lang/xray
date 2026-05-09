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
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xvalue.h"
#include <string.h>

/* ========== Constants ========== */

#define MAX_REGS 256
#define NO_REG   255

/* ========== Emit Context ========== */

typedef struct {
    XiFunc *func;
    XrProto *proto;
    struct XrayIsolate *isolate;      /* for string interning; may be NULL */
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

    /* OP_TRY patches: absolute target PC patching (catch + finally) */
    struct {
        int pc;              /* OP_TRY instruction PC */
        uint32_t target_bid; /* catch block ID */
        uint32_t finally_bid;/* finally block ID (0 if none) */
    } *try_patches;
    uint32_t ntry_patch;
    uint32_t try_patch_cap;

    /* Track which value IDs have been wrapped in a cell (OP_CELL_NEW).
     * Prevents double-wrapping when multiple closures capture the same
     * mutable variable. */
    bool *cell_wrapped;      /* [next_value_id] */

    /* Per-value bytecode PC: records the instruction offset for IC-relevant
     * ops (GETPROP/SETPROP/INVOKE).  Indexed by value_id; -1 = no mapping.
     * Consumed by build_slot_map() for JIT IC-guided speculation. */
    int *value_pc;           /* [next_value_id] */

    /* Comparison-branch fusion: if the block control is a comparison with
     * no other consumers, skip emitting OP_CMP_* and instead emit the
     * branch-form opcode (OP_LT/LE/EQ) directly in the terminator. */
    XiValue *fused_cmp;

    /* Side cell register map: maps var_id → cell register for hoisted
     * function captures.  The cell is allocated in a separate register
     * so the original register remains usable for direct parent calls.
     * NO_REG (0xFF) means no cell allocated for this variable. */
    uint8_t cell_side_reg[MAX_REGS];  /* var_id → cell register */

    /* Tracks whether OP_CELL_NEW has been emitted for a given var_id.
     * First write to a cell_side_reg variable emits CELL_NEW; subsequent
     * writes emit CELL_SET. */
    bool cell_created[MAX_REGS];      /* var_id → true if CELL_NEW emitted */

    /* Variable-based register coalescing: all SSA definitions of the same
     * source variable share one VM register.  This is required for correct
     * exception semantics — the VM's OP_THROW bypasses SSA phi resolution,
     * so catch-block modifications must write to the same register that
     * post-try-catch code reads from. */
    uint8_t var_reg[MAX_REGS];  /* var_id -> pinned register (NO_REG = unassigned) */
} EmitCtx;

/* ========== Shared Utility Functions ========== */

XR_FUNC void emit_error(EmitCtx *ctx, XiEmitStatus s);
XR_FUNC int current_pc(EmitCtx *ctx);
XR_FUNC void emit_inst(EmitCtx *ctx, XrInstruction inst);
XR_FUNC void free_reg(EmitCtx *ctx, uint8_t reg);
XR_FUNC uint8_t reg_of(EmitCtx *ctx, const XiValue *v);
XR_FUNC uint8_t reg_of_cell_deref(EmitCtx *ctx, const XiValue *v);
XR_FUNC uint8_t alloc_reg_fresh(EmitCtx *ctx, const XiValue *v);
XR_FUNC void try_free_args(EmitCtx *ctx, const XiValue *v);
XR_FUNC int add_const_int(EmitCtx *ctx, int64_t val);
XR_FUNC int add_const_float(EmitCtx *ctx, double val);
XR_FUNC int add_const_string(EmitCtx *ctx, const char *str);
XR_FUNC int add_symbol(EmitCtx *ctx, const char *name);
XR_FUNC void xi_emit_add_patch(EmitCtx *ctx, int pc, uint32_t target_bid);
XR_FUNC void add_try_patch(EmitCtx *ctx, int pc, uint32_t catch_bid,
                            uint32_t finally_bid);

/* ========== Functions from xi_emit_reg.c ========== */
XR_FUNC void compute_last_use(EmitCtx *ctx);
XR_FUNC void alloc_registers(EmitCtx *ctx);

/* ========== Functions from xi_emit_cf.c ========== */
XR_FUNC void emit_phi_moves(EmitCtx *ctx, XiBlock *pred, XiBlock *succ);
XR_FUNC void emit_block(EmitCtx *ctx, XiBlock *blk, XiBlock *next_blk);
XR_FUNC void patch_jumps(EmitCtx *ctx);

/* emit_value is defined in xi_emit.c (driver) and called by emit_block */
XR_FUNC void emit_value(EmitCtx *ctx, XiValue *v);

/* ========== Functions from xi_emit_slotmap.c ========== */
XR_FUNC XiSlotMap *build_slot_map(EmitCtx *ctx);

/* ========== Emit Handler Type ========== */

/* Signature for opcode-specific emission handlers.
 * ctx:  emitter context
 * v:    the Xi IR value to emit
 * dst:  pre-allocated destination register */
typedef void (*XiEmitHandler)(EmitCtx *ctx, XiValue *v, uint8_t dst);

/* Handler table: indexed by XiOp, NULL entries fall back to error. */
extern const XiEmitHandler xi_emit_handlers[XI_OP_COUNT];

/* ========== Handler Declarations (xi_emit_arith.c) ========== */
XR_FUNC void xi_emit_arith(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_neg(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_not(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_bnot(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_cmp(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_convert(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_box(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_unbox(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_narrow(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_widen(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_isnull(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_is(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_as(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_typeof(EmitCtx *ctx, XiValue *v, uint8_t dst);

/* ========== Handler Declarations (xi_emit_call.c) ========== */
XR_FUNC void xi_emit_call(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_extract(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_multi_ret(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_call_method(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_call_builtin(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_str_concat(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_print(EmitCtx *ctx, XiValue *v, uint8_t dst);

/* ========== Handler Declarations (xi_emit_object.c) ========== */
XR_FUNC void xi_emit_load_field(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_store_field(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_index_get(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_index_set(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_array_new(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_map_new(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_set_new(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_json_new(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_json_init_f(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_json_get_f(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_json_set_f(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_json_decode(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_range(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_slice(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_closure_new(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_load_upval(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_store_upval(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_get_shared(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_set_shared(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_get_builtin(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_iter(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_class_create(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_import_ref(EmitCtx *ctx, XiValue *v, uint8_t dst);

/* ========== Handler Declarations (xi_emit_eh.c) ========== */
XR_FUNC void xi_emit_throw(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_try(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_catch(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_finally(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_end_try(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_defer(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_go(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_await(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_yield(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_chan_new(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_chan_send(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_chan_recv(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_chan_try_send(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_chan_try_recv(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_scope_enter(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_scope_exit(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_assert(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_assert_eq(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_assert_ne(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_assert_throws(EmitCtx *ctx, XiValue *v, uint8_t dst);
XR_FUNC void xi_emit_regex_compile(EmitCtx *ctx, XiValue *v, uint8_t dst);

#endif  // XI_EMIT_INTERNAL_H
