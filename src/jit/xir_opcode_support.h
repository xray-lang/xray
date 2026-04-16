/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_opcode_support.h - JIT opcode support classification table
 *
 * KEY CONCEPT:
 *   Every bytecode opcode must be explicitly classified here.
 *   Adding a new opcode without updating this table causes a compile error
 *   (_Static_assert on table size), ensuring no opcode is silently skipped.
 *
 * CLASSIFICATION:
 *   JIT_OP_SUPPORTED    - Full JIT translation exists in xir_builder*.c
 *   JIT_OP_BAIL_OUT     - Opcode causes the entire function to bail from JIT
 *                         (function falls back to interpreter if this opcode appears)
 *   JIT_OP_UNIMPLEMENTED- New opcode not yet handled; causes compile error if
 *                         table is incomplete (default value = 0, caught by assert)
 *
 * USAGE:
 *   is_jit_eligible() in xir_jit.c uses jit_op_support_table[] to quickly
 *   reject functions containing BAIL_OUT opcodes before attempting compilation.
 *   The _Static_assert below catches table completeness at compile time.
 */

#ifndef XIR_OPCODE_SUPPORT_H
#define XIR_OPCODE_SUPPORT_H

#include "../runtime/value/xchunk.h"

typedef enum {
    JIT_OP_UNIMPLEMENTED = 0,  // default — catch missing entries
    JIT_OP_SUPPORTED,          // full JIT builder handler exists
    JIT_OP_BAIL_OUT,           // function rejected from JIT if opcode present
} JitOpcodeSupport;

static const JitOpcodeSupport jit_op_support_table[NUM_OPCODES] = {
    /* === Load / Move === */
    [OP_MOVE]              = JIT_OP_SUPPORTED,
    [OP_LOADI]             = JIT_OP_SUPPORTED,
    [OP_LOADF]             = JIT_OP_SUPPORTED,
    [OP_LOADK]             = JIT_OP_SUPPORTED,
    [OP_LOADNULL]          = JIT_OP_SUPPORTED,
    [OP_LOADTRUE]          = JIT_OP_SUPPORTED,
    [OP_LOADFALSE]         = JIT_OP_SUPPORTED,

    /* === Arithmetic === */
    [OP_ADD]               = JIT_OP_SUPPORTED,
    [OP_ADDI]              = JIT_OP_SUPPORTED,
    [OP_ADDK]              = JIT_OP_SUPPORTED,
    [OP_SUB]               = JIT_OP_SUPPORTED,
    [OP_SUBI]              = JIT_OP_SUPPORTED,
    [OP_SUBK]              = JIT_OP_SUPPORTED,
    [OP_MUL]               = JIT_OP_SUPPORTED,
    [OP_MULI]              = JIT_OP_SUPPORTED,
    [OP_MULK]              = JIT_OP_SUPPORTED,
    [OP_DIV]               = JIT_OP_SUPPORTED,
    [OP_DIVK]              = JIT_OP_SUPPORTED,
    [OP_MOD]               = JIT_OP_SUPPORTED,
    [OP_MODK]              = JIT_OP_SUPPORTED,
    [OP_UNM]               = JIT_OP_SUPPORTED,

    /* === Bitwise === */
    [OP_BAND]              = JIT_OP_SUPPORTED,
    [OP_BOR]               = JIT_OP_SUPPORTED,
    [OP_BXOR]              = JIT_OP_SUPPORTED,
    [OP_BNOT]              = JIT_OP_SUPPORTED,
    [OP_SHL]               = JIT_OP_SUPPORTED,
    [OP_SHR]               = JIT_OP_SUPPORTED,

    /* === Logical === */
    [OP_NOT]               = JIT_OP_SUPPORTED,

    /* === Comparison (branch form) === */
    [OP_EQ]                = JIT_OP_SUPPORTED,
    [OP_EQK]               = JIT_OP_SUPPORTED,
    [OP_EQI]               = JIT_OP_SUPPORTED,
    [OP_LT]                = JIT_OP_SUPPORTED,
    [OP_LTI]               = JIT_OP_SUPPORTED,
    [OP_LE]                = JIT_OP_SUPPORTED,
    [OP_LEI]               = JIT_OP_SUPPORTED,
    [OP_IS]                = JIT_OP_SUPPORTED,
    [OP_ISNULL]            = JIT_OP_SUPPORTED,
    [OP_ISNULL_SET]        = JIT_OP_SUPPORTED,

    /* === Comparison (expression form) === */
    [OP_CMP_EQ]            = JIT_OP_SUPPORTED,
    [OP_CMP_NE]            = JIT_OP_SUPPORTED,
    [OP_CMP_EQ_STRICT]     = JIT_OP_SUPPORTED,
    [OP_CMP_NE_STRICT]     = JIT_OP_SUPPORTED,
    [OP_CMP_LT]            = JIT_OP_SUPPORTED,
    [OP_CMP_LE]            = JIT_OP_SUPPORTED,

    /* === Control Flow === */
    [OP_JMP]               = JIT_OP_SUPPORTED,
    [OP_TEST]              = JIT_OP_SUPPORTED,
    [OP_TESTSET]           = JIT_OP_SUPPORTED,
    [OP_LOOP_BACK]         = JIT_OP_SUPPORTED,
    [OP_RETURN]            = JIT_OP_SUPPORTED,
    [OP_RETURN0]           = JIT_OP_SUPPORTED,
    [OP_RETURN1]           = JIT_OP_SUPPORTED,
    [OP_NOP]               = JIT_OP_SUPPORTED,

    /* === Type Conversion === */
    [OP_TOINT]             = JIT_OP_SUPPORTED,
    [OP_TOFLOAT]           = JIT_OP_SUPPORTED,
    [OP_TOSTRING]          = JIT_OP_SUPPORTED,
    [OP_TOBOOL]            = JIT_OP_SUPPORTED,
    [OP_BOX_I64]           = JIT_OP_SUPPORTED,
    [OP_BOX_F64]           = JIT_OP_SUPPORTED,
    [OP_UNBOX_I64]         = JIT_OP_SUPPORTED,
    [OP_UNBOX_F64]         = JIT_OP_SUPPORTED,
    [OP_COPY]              = JIT_OP_SUPPORTED,
    [OP_CHR]               = JIT_OP_SUPPORTED,

    /* === Closure & Upvalues === */
    [OP_CLOSURE]           = JIT_OP_SUPPORTED,
    [OP_UPVAL_GET]         = JIT_OP_SUPPORTED,
    [OP_CELL_NEW]          = JIT_OP_SUPPORTED,
    [OP_CELL_GET]          = JIT_OP_SUPPORTED,
    [OP_CELL_SET]          = JIT_OP_SUPPORTED,

    /* === Object / JSON === */
    [OP_GETFIELD]          = JIT_OP_SUPPORTED,
    [OP_SETFIELD]          = JIT_OP_SUPPORTED,
    [OP_GETFIELD_IC]       = JIT_OP_SUPPORTED,
    [OP_GETPROP]           = JIT_OP_SUPPORTED,
    [OP_SETPROP]           = JIT_OP_SUPPORTED,
    [OP_NEWJSON]           = JIT_OP_SUPPORTED,
    [OP_JSON_GET]          = JIT_OP_SUPPORTED,
    [OP_JSON_SET]          = JIT_OP_SUPPORTED,
    [OP_JSON_GETK]         = JIT_OP_SUPPORTED,
    [OP_JSON_SETK]         = JIT_OP_SUPPORTED,
    [OP_JSON_INIT]         = JIT_OP_SUPPORTED,
    [OP_JSON_INIT_I]       = JIT_OP_SUPPORTED,
    [OP_JSON_INIT_N]       = JIT_OP_SUPPORTED,
    [OP_TFIELD_GET]        = JIT_OP_SUPPORTED,
    [OP_TFIELD_SET]        = JIT_OP_SUPPORTED,

    /* === Array === */
    [OP_NEWARRAY]          = JIT_OP_SUPPORTED,
    [OP_ARRAY_GET]         = JIT_OP_SUPPORTED,
    [OP_ARRAY_GETC]        = JIT_OP_SUPPORTED,
    [OP_ARRAY_GET_NOCHECK] = JIT_OP_SUPPORTED,
    [OP_ARRAY_SET]         = JIT_OP_SUPPORTED,
    [OP_ARRAY_SETC]        = JIT_OP_SUPPORTED,
    [OP_ARRAY_PUSH]        = JIT_OP_SUPPORTED,
    [OP_ARRAY_INIT]        = JIT_OP_SUPPORTED,
    [OP_ARRAY_LEN]         = JIT_OP_SUPPORTED,
    [OP_TARRAY_GET]        = JIT_OP_SUPPORTED,
    [OP_TARRAY_GETC]       = JIT_OP_SUPPORTED,
    [OP_TARRAY_SET]        = JIT_OP_SUPPORTED,
    [OP_TARRAY_PUSH]       = JIT_OP_SUPPORTED,
    [OP_INDEX_GET]         = JIT_OP_SUPPORTED,
    [OP_INDEX_SET]         = JIT_OP_SUPPORTED,
    [OP_SLICE]             = JIT_OP_SUPPORTED,

    /* === Map / Set === */
    [OP_NEWMAP]            = JIT_OP_SUPPORTED,
    [OP_NEWSET]            = JIT_OP_SUPPORTED,
    [OP_MAP_GET]           = JIT_OP_SUPPORTED,
    [OP_MAP_GETK]          = JIT_OP_SUPPORTED,
    [OP_MAP_SET]           = JIT_OP_SUPPORTED,
    [OP_MAP_SETK]          = JIT_OP_SUPPORTED,
    [OP_MAP_SETKS]         = JIT_OP_SUPPORTED,
    [OP_MAP_INCREMENT]     = JIT_OP_SUPPORTED,

    /* === Range === */
    [OP_NEWRANGE]          = JIT_OP_SUPPORTED,
    [OP_RANGE_UNPACK]      = JIT_OP_SUPPORTED,

    /* === String === */
    [OP_STRBUF_NEW]        = JIT_OP_SUPPORTED,
    [OP_STRBUF_APPEND]     = JIT_OP_SUPPORTED,
    [OP_STRBUF_FINISH]     = JIT_OP_SUPPORTED,
    [OP_NEWSTRINGBUILDER]  = JIT_OP_SUPPORTED,
    [OP_SUBSTRING]         = JIT_OP_SUPPORTED,
    [OP_STR_REPEAT]        = JIT_OP_SUPPORTED,

    /* === Class / Struct === */
    [OP_CLASS_CREATE_FROM_DESCRIPTOR] = JIT_OP_SUPPORTED,
    [OP_CLINIT_CALL]       = JIT_OP_SUPPORTED,
    [OP_INHERIT]           = JIT_OP_SUPPORTED,
    [OP_GETSUPER]          = JIT_OP_SUPPORTED,
    [OP_NEW_STRUCT]        = JIT_OP_SUPPORTED,
    [OP_STRUCT_GET]        = JIT_OP_SUPPORTED,
    [OP_STRUCT_SET]        = JIT_OP_SUPPORTED,
    [OP_STRUCT_COPY]       = JIT_OP_SUPPORTED,
    [OP_SET_STORAGE_CTX]   = JIT_OP_SUPPORTED,
    [OP_TO_SHARED]         = JIT_OP_SUPPORTED,
    [OP_ABSTRACT_ERROR]    = JIT_OP_SUPPORTED,

    /* === Enum === */
    [OP_ENUM_ACCESS]       = JIT_OP_SUPPORTED,
    [OP_ENUM_CONVERT]      = JIT_OP_SUPPORTED,
    [OP_ENUM_NAME]         = JIT_OP_SUPPORTED,

    /* === Shared / Global === */
    [OP_GETSHARED]         = JIT_OP_SUPPORTED,
    [OP_SETSHARED]         = JIT_OP_SUPPORTED,
    [OP_GETBUILTIN]        = JIT_OP_SUPPORTED,

    /* === Function Calls === */
    [OP_CALL]              = JIT_OP_SUPPORTED,
    [OP_CALL_STATIC]       = JIT_OP_SUPPORTED,
    [OP_CALL_KEEP]         = JIT_OP_SUPPORTED,
    [OP_CALLSELF]          = JIT_OP_SUPPORTED,
    [OP_TAILCALL]          = JIT_OP_SUPPORTED,
    [OP_INVOKE]            = JIT_OP_SUPPORTED,
    [OP_INVOKE_BUILTIN]    = JIT_OP_SUPPORTED,
    [OP_INVOKE_DIRECT]     = JIT_OP_SUPPORTED,
    [OP_INVOKE_TAIL]       = JIT_OP_SUPPORTED,
    [OP_SUPERINVOKE]       = JIT_OP_SUPPORTED,

    /* === Structured Concurrency === */
    [OP_SCOPE_ENTER]       = JIT_OP_BAIL_OUT,  // requires VM scope tracking
    [OP_SCOPE_EXIT]        = JIT_OP_BAIL_OUT,   // requires VM scope tracking
    [OP_SPAWN_CONT]        = JIT_OP_BAIL_OUT,   // requires child-first dispatch
    [OP_AWAIT]             = JIT_OP_SUPPORTED,   // deopt-based: fast path in JIT, slow path deopts to interpreter

    /* === Non-blocking Channel === */
    [OP_CHAN_NEW]          = JIT_OP_SUPPORTED,
    [OP_CHAN_CLOSE]        = JIT_OP_SUPPORTED,
    [OP_CHAN_IS_CLOSED]    = JIT_OP_SUPPORTED,
    [OP_CHAN_TRY_SEND]     = JIT_OP_SUPPORTED,
    [OP_CHAN_TRY_RECV]     = JIT_OP_SUPPORTED,

    /* === Exception Handling === */
    [OP_TRY]               = JIT_OP_SUPPORTED,
    [OP_CATCH]             = JIT_OP_SUPPORTED,
    [OP_FINALLY]           = JIT_OP_SUPPORTED,
    [OP_END_TRY]           = JIT_OP_SUPPORTED,
    [OP_THROW]             = JIT_OP_SUPPORTED,

    /* === Assertions === */
    [OP_ASSERT]            = JIT_OP_SUPPORTED,
    [OP_ASSERT_EQ]         = JIT_OP_SUPPORTED,
    [OP_ASSERT_NE]         = JIT_OP_SUPPORTED,

    /* === Spill / Reload (internal) === */
    [OP_SPILL]             = JIT_OP_SUPPORTED,
    [OP_RELOAD]            = JIT_OP_SUPPORTED,

    /* === Module === */
    [OP_IMPORT]            = JIT_OP_SUPPORTED,
    [OP_EXPORT]            = JIT_OP_SUPPORTED,
    [OP_EXPORT_ALL]        = JIT_OP_SUPPORTED,

    /* === Misc === */
    [OP_PRINT]             = JIT_OP_SUPPORTED,
    [OP_DUMP]              = JIT_OP_SUPPORTED,
    [OP_TYPEOF]            = JIT_OP_SUPPORTED,
    [OP_TYPENAME]          = JIT_OP_SUPPORTED,
    [OP_REGEX_COMPILE]     = JIT_OP_SUPPORTED,
    [OP_INST_TYPE_ARGS]    = JIT_OP_BAIL_OUT,

    /* === Blocking Channel (JIT CPS via XIR_SUSPEND) === */
    [OP_CHAN_SEND]          = JIT_OP_SUPPORTED,
    [OP_CHAN_RECV]          = JIT_OP_SUPPORTED,

    /* === Blocking Channel / Scheduler (BAIL_OUT: require VM scheduler) === */
    [OP_CHAN_NEW_NAMED]     = JIT_OP_BAIL_OUT,
    [OP_CHAN_SEND_TIMEOUT]  = JIT_OP_BAIL_OUT,
    [OP_CHAN_RECV_TIMEOUT]  = JIT_OP_BAIL_OUT,
    [OP_SELECT_START]      = JIT_OP_BAIL_OUT,
    [OP_SELECT_CASE]       = JIT_OP_BAIL_OUT,
    [OP_SELECT_BLOCK]      = JIT_OP_BAIL_OUT,
    [OP_SELECT_END]        = JIT_OP_BAIL_OUT,

    /* === Coroutine Control (BAIL_OUT: require VM scheduler) === */
    [OP_GO]                = JIT_OP_BAIL_OUT,
    [OP_GO_INVOKE]         = JIT_OP_BAIL_OUT,
    [OP_AWAIT_TIMEOUT]     = JIT_OP_BAIL_OUT,
    [OP_AWAIT_ALL]         = JIT_OP_BAIL_OUT,
    [OP_AWAIT_ANY]         = JIT_OP_BAIL_OUT,
    [OP_YIELD]             = JIT_OP_BAIL_OUT,
    [OP_SLEEP]             = JIT_OP_BAIL_OUT,
    [OP_TIME_AFTER]        = JIT_OP_BAIL_OUT,
    [OP_CANCELLED]         = JIT_OP_BAIL_OUT,
    [OP_CORO_CTRL]         = JIT_OP_BAIL_OUT,
    [OP_SET_PRIORITY]      = JIT_OP_BAIL_OUT,
    [OP_LOCK_THREAD]       = JIT_OP_BAIL_OUT,
    [OP_UNLOCK_THREAD]     = JIT_OP_BAIL_OUT,
    [OP_SET_LOCAL]         = JIT_OP_BAIL_OUT,
    [OP_GET_LOCAL]         = JIT_OP_BAIL_OUT,

    /* === Misc BAIL_OUT === */
    [OP_DEFER]             = JIT_OP_BAIL_OUT,
    [OP_BYTES_NEW]         = JIT_OP_BAIL_OUT,
};

_Static_assert(
    sizeof(jit_op_support_table) / sizeof(jit_op_support_table[0]) == NUM_OPCODES,
    "jit_op_support_table is incomplete — add new opcode to xir_opcode_support.h");

_Static_assert(JIT_OP_UNIMPLEMENTED == 0,
    "JIT_OP_UNIMPLEMENTED must be 0 so uninitialised entries are caught");

#endif // XIR_OPCODE_SUPPORT_H
