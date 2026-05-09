/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_backend.h - Backend-legal op classification
 *
 * Defines which XiOp opcodes are legal at XI_STAGE_BACKEND.
 * Any op NOT in the whitelist must be expanded by xi_backend_lower()
 * before code generation.
 *
 * Design principle: BACKEND ops are either:
 *   (a) Primitive (arithmetic, bitwise, comparison, control flow)
 *   (b) Explicit runtime dispatch (XI_CALL, XI_CALL_METHOD, XI_CALL_BUILTIN)
 *   (c) Structural (PHI, COPY, SELECT, BOX, UNBOX, CONVERT)
 *
 * High-level semantic ops (PRINT, ITER_*, ARRAY_NEW, STR_CONCAT, etc.)
 * are syntactic sugar that must be lowered to (b) before backend.
 */

#ifndef XI_BACKEND_H
#define XI_BACKEND_H

#include "xi.h"
#include <stdbool.h>

/* Returns true if the opcode is legal at STAGE_BACKEND.
 * Non-legal ops must be eliminated by xi_backend_lower(). */
static inline bool xi_op_is_backend_legal(uint16_t op) {
    switch ((XiOp)op) {
    /* Constants & parameters */
    case XI_CONST:
    case XI_PARAM:

    /* Arithmetic (polymorphic int/float via rep) */
    case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
    case XI_NEG:

    /* Bitwise */
    case XI_BAND: case XI_BOR: case XI_BXOR: case XI_BNOT:
    case XI_SHL: case XI_SHR:

    /* Comparison */
    case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
    case XI_EQ_STRICT: case XI_NE_STRICT:

    /* Logical */
    case XI_NOT:

    /* Type conversion & representation */
    case XI_CONVERT:
    case XI_BOX:
    case XI_UNBOX:

    /* Explicit narrowing/widening (native-width truncation/extension) */
    case XI_NARROW_I8: case XI_NARROW_U8:
    case XI_NARROW_I16: case XI_NARROW_U16:
    case XI_NARROW_I32: case XI_NARROW_U32:
    case XI_NARROW_F32:
    case XI_WIDEN_I8: case XI_WIDEN_U8:
    case XI_WIDEN_I16: case XI_WIDEN_U16:
    case XI_WIDEN_I32: case XI_WIDEN_U32:
    case XI_WIDEN_F32:

    /* Field/index access (direct runtime dispatch) */
    case XI_LOAD_FIELD: case XI_STORE_FIELD:
    case XI_INDEX_GET: case XI_INDEX_SET:

    /* Call family (the universal lowering target) */
    case XI_CALL:
    case XI_CALL_METHOD:
    case XI_CALL_BUILTIN:
    case XI_EXTRACT:

    /* Closure & upvalue */
    case XI_CLOSURE_NEW:
    case XI_LOAD_UPVAL: case XI_STORE_UPVAL:

    /* Module-level state */
    case XI_GET_SHARED: case XI_SET_SHARED:

    /* Coroutine (low-level runtime ops) */
    case XI_GO: case XI_AWAIT: case XI_YIELD:
    case XI_CHAN_SEND: case XI_CHAN_RECV:
    case XI_CHAN_TRY_SEND: case XI_CHAN_TRY_RECV:
    case XI_CHAN_NEW:

    /* Exception handling */
    case XI_THROW:
    case XI_TRY: case XI_CATCH: case XI_FINALLY: case XI_END_TRY:

    /* Control flow / SSA structural */
    case XI_PHI:
    case XI_SELECT:
    case XI_COPY:
    case XI_ISNULL:
    case XI_MULTI_RET:

    /* OOP & scope */
    case XI_CLASS_CREATE:
    case XI_SCOPE_ENTER: case XI_SCOPE_EXIT:
    case XI_DEFER:

    /* Type ops (runtime dispatch) */
    case XI_IS: case XI_AS:

    /* Assertions (aux carries location string — cannot rewrite to
     * XI_CALL_BUILTIN without losing the string). */
    case XI_ASSERT: case XI_ASSERT_EQ: case XI_ASSERT_NE:
    case XI_ASSERT_THROWS:

    /* Json (XI_JSON_NEW/DECODE: aux carries field_names pointer) */
    case XI_JSON_NEW: case XI_JSON_DECODE:

    /* Builtin global load (aux carries name string) */
    case XI_GET_BUILTIN:

    /* Cross-module */
    case XI_IMPORT_REF:

    /* ARC / Ownership (inserted after escape analysis) */
    case XI_RETAIN: case XI_RELEASE: case XI_MOVE:

    /* Stack allocation (replaces heap alloc for NO_ESCAPE values) */
    case XI_STACK_ALLOC:
        return true;

    /* --- Non-legal (must be lowered before BACKEND) --- */
    /* XI_PRINT         → XI_CALL_BUILTIN(print)          */
    /* XI_STR_CONCAT    → XI_CALL_BUILTIN(str_concat)     */
    /* XI_ARRAY_NEW     → XI_CALL_BUILTIN(array_new)      */
    /* XI_MAP_NEW       → XI_CALL_BUILTIN(map_new)        */
    /* XI_SET_NEW       → XI_CALL_BUILTIN(set_new)        */
    /* XI_JSON_INIT_F   → XI_CALL_BUILTIN(json_init_f)    */
    /* XI_JSON_GET_F    → XI_CALL_BUILTIN(json_get_f)     */
    /* XI_JSON_SET_F    → XI_CALL_BUILTIN(json_set_f)     */
    /* XI_ITER_NEW      → XI_CALL_BUILTIN(iter_new)       */
    /* XI_ITER_NEXT     → XI_CALL_BUILTIN(iter_next)      */
    /* XI_ITER_VALID    → XI_CALL_BUILTIN(iter_valid)     */
    /* XI_SLICE         → XI_CALL_BUILTIN(slice)          */
    /* XI_RANGE         → XI_CALL_BUILTIN(range)          */
    /* XI_TYPEOF        → XI_CALL_BUILTIN(typeof)         */
    /* XI_REGEX_COMPILE → XI_CALL_BUILTIN(regex_compile)  */
    default:
        return false;
    }
}

#endif  // XI_BACKEND_H
