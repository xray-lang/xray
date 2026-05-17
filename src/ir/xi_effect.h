/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_effect.h - Opcode-to-effect table for Xi IR
 *
 * Each Xi opcode has a set of default effects that describe its
 * observable behavior.  The lowerer ORs additional flags as needed
 * (e.g. MAY_THROW on integer division), but the opcode defaults
 * provide a sound baseline.
 *
 * Consumers:
 *   - xi_lower*.c    : seed value->flags from the table
 *   - xi_verify.c    : assert flags are superset of opcode defaults
 *   - xi_opt*.c      : query whether an op reads/writes memory
 *   - xi_pipeline.c  : compute per-function effect summary
 */

#ifndef XI_EFFECT_H
#define XI_EFFECT_H

#include "xi.h"

/* Return the default effect flags for a given XiOp.
 * These flags represent the *minimum* effects any instance of the op
 * must carry.  The lowerer may set additional flags (e.g. MAY_THROW
 * on a call that can throw). */
static inline uint8_t xi_op_default_effects(uint16_t op) {
    switch ((XiOp) op) {
        /* --- Pure, no effects --- */
        case XI_CONST:
        case XI_PARAM:
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_NEG:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_EQ_STRICT:
        case XI_NE_STRICT:
        case XI_NOT:
        case XI_CONVERT:
        case XI_BOX:
        case XI_UNBOX:
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
        case XI_WIDEN_F32:
        case XI_ISNULL:
        case XI_IS:
        case XI_AS:
        case XI_PHI:
        case XI_SELECT:
        case XI_COPY:
        case XI_RANGE:
        case XI_EXTRACT:
        case XI_MULTI_RET:
        case XI_TYPEOF:
            return 0;

        /* --- Reads heap memory --- */
        case XI_LOAD_FIELD:
        case XI_STRUCT_GET:
        case XI_INDEX_GET:
        case XI_JSON_GET_F:
        case XI_TUPLE_GET:
        case XI_GET_SHARED:
        case XI_GET_GLOBAL:
        case XI_LOAD_UPVAL:
        case XI_ITER_VALID:
        case XI_GET_BUILTIN:
        case XI_IMPORT_REF:
            return XI_FLAG_READS_MEM;

        /* --- Writes heap memory (implies side effect) --- */
        case XI_STORE_FIELD:
        case XI_STRUCT_SET:
        case XI_INDEX_SET:
        case XI_JSON_INIT_F:
        case XI_JSON_SET_F:
        case XI_SET_SHARED:
        case XI_SET_GLOBAL:
        case XI_STORE_UPVAL:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

        /* --- Allocation (read+write, side effect) --- */
        case XI_STRUCT_NEW:
        case XI_JSON_NEW:
        case XI_JSON_DECODE:
        case XI_ARRAY_NEW:
        case XI_MAP_NEW:
        case XI_TUPLE_NEW:
        case XI_SET_NEW:
        case XI_CHAN_NEW:
        case XI_CLOSURE_NEW:
        case XI_CLASS_CREATE:
        case XI_REGEX_COMPILE:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

        /* --- I/O and print --- */
        case XI_PRINT:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

        /* --- Calls: conservatively assume all effects --- */
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_BUILTIN:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;

        /* --- Iterator advancement: reads + writes + may throw --- */
        case XI_ITER_NEW:
        case XI_ITER_NEXT:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;

        /* --- String concat: allocation + reads --- */
        case XI_STR_CONCAT:
        case XI_SLICE:
            return XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;

        /* --- Exception handling --- */
        case XI_THROW:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        case XI_TRY:
        case XI_CATCH:
        case XI_FINALLY:
        case XI_END_TRY:
            return XI_FLAG_SIDE_EFFECT;

        /* --- Assert: side effect + may throw --- */
        case XI_ASSERT:
        case XI_ASSERT_EQ:
        case XI_ASSERT_NE:
        case XI_ASSERT_THROWS:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

        /* --- Coroutine ops: suspend + side effect --- */
        case XI_GO:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
        case XI_AWAIT:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND | XI_FLAG_MAY_THROW;
        case XI_YIELD:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;
        case XI_CHAN_SEND:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND | XI_FLAG_WRITES_MEM;
        case XI_CHAN_RECV:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND | XI_FLAG_READS_MEM;
        case XI_CHAN_TRY_SEND:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
        case XI_CHAN_TRY_RECV:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM;

        /* --- Coro built-in module methods --- */
        case XI_CORO_OP:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;

        /* --- Structured concurrency scope --- */
        case XI_SCOPE_ENTER:
            return XI_FLAG_SIDE_EFFECT;
        case XI_SCOPE_EXIT:
            return XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;

        /* --- Defer --- */
        case XI_DEFER:
            return XI_FLAG_SIDE_EFFECT;

        /* --- ARC / Ownership --- */
        case XI_RETAIN:
        case XI_RELEASE:
            return XI_FLAG_SIDE_EFFECT;
        case XI_MOVE:
            return 0; /* pure annotation: ownership transfer, no runtime effect */

        /* --- Stack allocation --- */
        case XI_STACK_ALLOC:
            return XI_FLAG_SIDE_EFFECT;

        case XI_OP_COUNT:
            break;
    }
    return 0;
}

/* Query helpers for optimization passes */

static inline bool xi_op_may_suspend(uint16_t op) {
    return (xi_op_default_effects(op) & XI_FLAG_MAY_SUSPEND) != 0;
}

static inline bool xi_op_reads_mem(uint16_t op) {
    return (xi_op_default_effects(op) & XI_FLAG_READS_MEM) != 0;
}

static inline bool xi_op_writes_mem(uint16_t op) {
    return (xi_op_default_effects(op) & XI_FLAG_WRITES_MEM) != 0;
}

static inline bool xi_op_is_pure(uint16_t op) {
    return xi_op_default_effects(op) == 0;
}

#endif  // XI_EFFECT_H
