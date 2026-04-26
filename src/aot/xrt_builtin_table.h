/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_builtin_table.h - Builtin method metadata (single source of truth)
 *
 * Every AOT-supported builtin method has exactly one entry here.
 * The codegen (xcgen_call.c) and runtime (xrt_method.h) both derive
 * from this table. Adding a method = add one X-macro entry + implement
 * in xrt_method.h.
 *
 * Consistency: XRT_BUILTIN_COUNT static-asserted against table size.
 */

#ifndef XRT_BUILTIN_TABLE_H
#define XRT_BUILTIN_TABLE_H

#include "xrt_method.h"

/* =========================================================================
 * Receiver type bitmask (for documentation / codegen hints)
 * ========================================================================= */

#define XRTM_STR    0x01
#define XRTM_ARR    0x02
#define XRTM_MAP    0x04
#define XRTM_I64    0x08
#define XRTM_F64    0x10
#define XRTM_BOOL   0x20
#define XRTM_ANY    0xFF

/* Return type hint */
#define XRT_RET_I64     0   /* int64_t */
#define XRT_RET_F64     1   /* double */
#define XRT_RET_STR     2   /* string */
#define XRT_RET_BOOL    3   /* bool (as int) */
#define XRT_RET_ARR     4   /* array */
#define XRT_RET_MAP     5   /* map */
#define XRT_RET_TAGGED  6   /* generic XrtValue */
#define XRT_RET_VOID    7   /* no meaningful return */
#define XRT_RET_SELF    8   /* returns receiver */

/* =========================================================================
 * X-macro table: XRT_BUILTIN_METHODS(X)
 *   X(SYM_SUFFIX, sym_id, name, nargs, recv_types, ret_type)
 *
 * SYM_SUFFIX: matches XRT_SYM_<SYM_SUFFIX>
 * sym_id:     numeric ID (must match SYMBOL_* enum in xsymbol_table.h)
 * name:       human-readable method name
 * nargs:      argument count excluding receiver
 * recv_types: bitmask of valid receiver types
 * ret_type:   XRT_RET_* hint
 * ========================================================================= */

#define XRT_BUILTIN_METHODS(X) \
    /* --- Common (multi-type) --- */ \
    X(LENGTH,       1,  "length",      0, XRTM_STR|XRTM_ARR|XRTM_MAP, XRT_RET_I64)  \
    X(IS_EMPTY,     2,  "isEmpty",     0, XRTM_STR|XRTM_ARR|XRTM_MAP, XRT_RET_BOOL) \
    X(HAS,          3,  "has",         1, XRTM_MAP,                     XRT_RET_BOOL) \
    X(GET,          4,  "get",         1, XRTM_MAP,                     XRT_RET_TAGGED) \
    X(SET,          5,  "set",         2, XRTM_MAP,                     XRT_RET_VOID) \
    X(DELETE,       6,  "delete",      1, XRTM_MAP,                     XRT_RET_BOOL) \
    X(CLEAR,        7,  "clear",       0, XRTM_MAP|XRTM_ARR,           XRT_RET_VOID) \
    X(KEYS,         8,  "keys",        0, XRTM_MAP,                     XRT_RET_ARR)  \
    X(VALUES,       9,  "values",      0, XRTM_MAP,                     XRT_RET_ARR)  \
    X(ENTRIES,     10,  "entries",      0, XRTM_MAP,                     XRT_RET_ARR)  \
    /* --- String methods --- */ \
    X(SLICE,       16,  "slice",       1, XRTM_STR|XRTM_ARR,           XRT_RET_SELF) \
    X(CHARAT,      17,  "charAt",      1, XRTM_STR,                     XRT_RET_STR)  \
    X(SUBSTRING,   18,  "substring",   2, XRTM_STR,                     XRT_RET_STR)  \
    X(INDEXOF,     19,  "indexOf",     1, XRTM_STR|XRTM_ARR,           XRT_RET_I64)  \
    X(CONTAINS,    20,  "contains",    1, XRTM_STR,                     XRT_RET_BOOL) \
    X(STARTSWITH,  21,  "startsWith",  1, XRTM_STR,                     XRT_RET_BOOL) \
    X(ENDSWITH,    22,  "endsWith",    1, XRTM_STR,                     XRT_RET_BOOL) \
    X(TOLOWER,     23,  "toLowerCase", 0, XRTM_STR,                     XRT_RET_STR)  \
    X(TOUPPER,     24,  "toUpperCase", 0, XRTM_STR,                     XRT_RET_STR)  \
    X(TRIM,        25,  "trim",        0, XRTM_STR,                     XRT_RET_STR)  \
    X(SPLIT,       26,  "split",       1, XRTM_STR,                     XRT_RET_ARR)  \
    X(REPLACE,     27,  "replace",     2, XRTM_STR,                     XRT_RET_STR)  \
    X(REPLACEALL,  28,  "replaceAll",  2, XRTM_STR,                     XRT_RET_STR)  \
    X(REPEAT,      29,  "repeat",      1, XRTM_STR,                     XRT_RET_STR)  \
    X(CONCAT,      30,  "concat",      1, XRTM_STR,                     XRT_RET_STR)  \
    X(BYTE_AT,     31,  "byteAt",      1, XRTM_STR,                     XRT_RET_I64)  \
    X(TRIM_START,  35,  "trimStart",   0, XRTM_STR,                     XRT_RET_STR)  \
    X(TRIM_END,    36,  "trimEnd",     0, XRTM_STR,                     XRT_RET_STR)  \
    X(PAD_START,   37,  "padStart",    2, XRTM_STR,                     XRT_RET_STR)  \
    X(PAD_END,     38,  "padEnd",      2, XRTM_STR,                     XRT_RET_STR)  \
    X(LASTINDEXOF, 39,  "lastIndexOf", 1, XRTM_STR,                     XRT_RET_I64)  \
    X(TOINT,       40,  "toInt",       0, XRTM_STR,                     XRT_RET_I64)  \
    X(TOFLOAT,     41,  "toFloat",     0, XRTM_STR,                     XRT_RET_F64)  \
    X(ORD,         46,  "ord",         0, XRTM_STR,                     XRT_RET_I64)  \
    /* --- Array methods --- */ \
    X(PUSH,        48,  "push",        1, XRTM_ARR,                     XRT_RET_VOID) \
    X(POP,         49,  "pop",         0, XRTM_ARR,                     XRT_RET_TAGGED) \
    X(SHIFT,       50,  "shift",       0, XRTM_ARR,                     XRT_RET_TAGGED) \
    X(UNSHIFT,     51,  "unshift",     1, XRTM_ARR,                     XRT_RET_VOID) \
    X(JOIN,        52,  "join",        1, XRTM_ARR,                     XRT_RET_STR)  \
    X(REVERSE,     53,  "reverse",     0, XRTM_ARR,                     XRT_RET_SELF) \
    /* --- Number methods --- */ \
    X(FLOOR,       58,  "floor",       0, XRTM_F64,                     XRT_RET_F64)  \
    X(CEIL,        59,  "ceil",        0, XRTM_F64,                     XRT_RET_F64)  \
    X(ROUND,       60,  "round",       0, XRTM_F64,                     XRT_RET_F64)  \
    X(ABS,         61,  "abs",         0, XRTM_I64|XRTM_F64,           XRT_RET_SELF) \
    X(SQRT,        62,  "sqrt",        0, XRTM_F64,                     XRT_RET_F64)  \
    X(POW,         63,  "pow",         1, XRTM_F64,                     XRT_RET_F64)  \
    X(TOFIXED,     64,  "toFixed",     1, XRTM_F64,                     XRT_RET_STR)  \
    X(MAX,         66,  "max",         1, XRTM_I64|XRTM_F64,           XRT_RET_SELF) \
    X(MIN,         67,  "min",         1, XRTM_I64|XRTM_F64,           XRT_RET_SELF) \
    X(TOHEX,       68,  "toHex",       0, XRTM_I64,                     XRT_RET_STR)  \
    /* --- Enum / type --- */ \
    X(TOSTRING,    85,  "toString",    0, XRTM_ANY,                     XRT_RET_STR)  \
    /* --- Array extended --- */ \
    X(FILL,       168,  "fill",        1, XRTM_ARR,                     XRT_RET_SELF) \
    X(SORT,       169,  "sort",        0, XRTM_ARR,                     XRT_RET_SELF) \
    X(INCLUDES,   170,  "includes",    1, XRTM_ARR,                     XRT_RET_BOOL) \
    X(CAPACITY,   171,  "capacity",    0, XRTM_ARR,                     XRT_RET_I64)  \

/* =========================================================================
 * Metadata struct + static table (generated from X-macro)
 * ========================================================================= */

typedef struct {
    uint16_t    symbol;
    const char *name;
    uint8_t     nargs;
    uint8_t     recv_types;
    uint8_t     ret_type;
} XrtBuiltinEntry;

/* Count entries at compile time */
#define XRT_COUNT_ONE(...) +1
#define XRT_BUILTIN_COUNT (0 XRT_BUILTIN_METHODS(XRT_COUNT_ONE))

/* Static table array */
#define XRT_ENTRY(SYM, ID, NAME, NARGS, RECV, RET) \
    { (ID), (NAME), (NARGS), (RECV), (RET) },

static const XrtBuiltinEntry xrt_builtin_entries[] = {
    XRT_BUILTIN_METHODS(XRT_ENTRY)
};

/* Linear lookup (table is small, <50 entries) */
static inline const XrtBuiltinEntry *xrt_builtin_find(int symbol) {
    for (int i = 0; i < XRT_BUILTIN_COUNT; i++) {
        if (xrt_builtin_entries[i].symbol == (uint16_t)symbol)
            return &xrt_builtin_entries[i];
    }
    return NULL;
}

/* =========================================================================
 * Compile-time consistency check
 * ========================================================================= */

#if __STDC_VERSION__ >= 201112L
#include <assert.h>
_Static_assert(sizeof(xrt_builtin_entries) / sizeof(xrt_builtin_entries[0])
               == (0 XRT_BUILTIN_METHODS(XRT_COUNT_ONE)),
               "xrt_builtin_entries count mismatch");
#endif

#endif // XRT_BUILTIN_TABLE_H
