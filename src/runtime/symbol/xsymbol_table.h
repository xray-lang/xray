/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsymbol_table.h - Symbol table for method/property name interning
 *
 * KEY CONCEPT:
 *   Maps strings to unique IDs for O(1) lookup.
 *   Builtin symbol IDs are compile-time constants (enum).
 *   Runtime-registered symbols get IDs beyond SYMBOL_BUILTIN_COUNT.
 *   Uses hash table (name->id) + array (id->name).
 *
 * WHY THIS DESIGN:
 *   - Enum constants are multi-isolate safe (no global mutable state)
 *   - Table-driven registration eliminates duplicate/zombie symbols
 *   - Compile-time IDs enable switch-case optimization
 */

#ifndef XSYMBOL_TABLE_H
#define XSYMBOL_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../base/xhashmap.h"

/* ========== Symbol ID Type ========== */

typedef int32_t SymbolId;

/*
 * Builtin symbol IDs — compile-time constants.
 * Order must match xr_builtin_symbol_names[] in xsymbol_table.c.
 * ID = enum value (starts from 1, 0 is SYMBOL_INVALID).
 */
enum {
    SYMBOL_INVALID = 0,

    // Common methods (Array/Map/Set/String)
    SYMBOL_LENGTH,
    SYMBOL_IS_EMPTY,
    SYMBOL_HAS,
    SYMBOL_GET,
    SYMBOL_SET,
    SYMBOL_DELETE,
    SYMBOL_CLEAR,
    SYMBOL_KEYS,
    SYMBOL_VALUES,
    SYMBOL_ENTRIES,

    // Functional methods
    SYMBOL_HAS_VALUE_MAP,
    SYMBOL_FOREACH,
    SYMBOL_MAP,
    SYMBOL_FILTER,
    SYMBOL_REDUCE,

    // String methods
    SYMBOL_SLICE,
    SYMBOL_CHARAT,
    SYMBOL_SUBSTRING,
    SYMBOL_INDEXOF,
    SYMBOL_CONTAINS,
    SYMBOL_STARTSWITH,
    SYMBOL_ENDSWITH,
    SYMBOL_TOLOWERCASE,
    SYMBOL_TOUPPERCASE,
    SYMBOL_TRIM,
    SYMBOL_SPLIT,
    SYMBOL_REPLACE,
    SYMBOL_REPLACEALL,
    SYMBOL_REPEAT,
    SYMBOL_CONCAT,
    SYMBOL_BYTE_AT,
    SYMBOL_REVERSE_BYTES,
    SYMBOL_TRANSLATE,
    SYMBOL_TRANSLATE_BYTES,
    SYMBOL_TRIM_START,
    SYMBOL_TRIM_END,
    SYMBOL_PAD_START,
    SYMBOL_PAD_END,
    SYMBOL_LASTINDEXOF,
    SYMBOL_TOINT,
    SYMBOL_TOFLOAT,
    SYMBOL_IS_LETTER,
    SYMBOL_IS_NUMBER,
    SYMBOL_IS_ALNUM,
    SYMBOL_IS_WHITESPACE,
    SYMBOL_ORD,
    SYMBOL_MATCH,

    // Array methods
    SYMBOL_PUSH,
    SYMBOL_POP,
    SYMBOL_SHIFT,
    SYMBOL_UNSHIFT,
    SYMBOL_JOIN,
    SYMBOL_REVERSE,

    // Iterator methods
    SYMBOL_ITERATOR,
    SYMBOL_ENTRIES_ITERATOR,
    SYMBOL_HASNEXT,
    SYMBOL_NEXT,

    // Number methods
    SYMBOL_FLOOR,
    SYMBOL_CEIL,
    SYMBOL_ROUND,
    SYMBOL_ABS,
    SYMBOL_SQRT,
    SYMBOL_POW,
    SYMBOL_TOFIXED,
    SYMBOL_TOBIGINT,
    SYMBOL_MAX,
    SYMBOL_MIN,
    SYMBOL_TOHEX,

    // UTF-8 methods
    SYMBOL_CHAR_LENGTH,
    SYMBOL_BYTE_LENGTH,
    SYMBOL_CODEPOINT_AT,
    SYMBOL_FROM_CODEPOINT,
    SYMBOL_CHARS,

    // Set methods
    SYMBOL_ADD,
    SYMBOL_UNION,
    SYMBOL_INTERSECTION,
    SYMBOL_DIFFERENCE,
    SYMBOL_SYMMETRIC_DIFFERENCE,
    SYMBOL_TO_ARRAY,
    SYMBOL_IS_SUBSET,
    SYMBOL_IS_SUPERSET,

    // Enum/type methods
    SYMBOL_NAME,
    SYMBOL_VALUE,
    SYMBOL_ORDINAL,
    SYMBOL_TOSTRING,
    SYMBOL_MEMBER_COUNT,
    SYMBOL_GET_MEMBER,

    // DateTime methods
    SYMBOL_FORMAT,
    SYMBOL_YEAR,
    SYMBOL_MONTH,
    SYMBOL_DAY,
    SYMBOL_HOUR,
    SYMBOL_MINUTE,
    SYMBOL_SECOND,
    SYMBOL_WEEKDAY,
    SYMBOL_TIMESTAMP,
    SYMBOL_TO_UTC,
    SYMBOL_TO_LOCAL,
    SYMBOL_MILLISECOND,
    SYMBOL_IS_BEFORE,
    SYMBOL_IS_AFTER,
    SYMBOL_EQUALS,
    SYMBOL_IS_LEAP_YEAR,
    SYMBOL_DAYS_IN_MONTH,
    SYMBOL_TO_ISO_STRING,
    SYMBOL_YEARDAY,

    // Operator overload symbols
    SYMBOL_OP_ADD,
    SYMBOL_OP_SUB,
    SYMBOL_OP_MUL,
    SYMBOL_OP_DIV,
    SYMBOL_OP_MOD,
    SYMBOL_OP_EQ,
    SYMBOL_OP_NE,
    SYMBOL_OP_LT,
    SYMBOL_OP_LE,
    SYMBOL_OP_GT,
    SYMBOL_OP_GE,
    SYMBOL_OP_BAND,
    SYMBOL_OP_BOR,
    SYMBOL_OP_BXOR,
    SYMBOL_OP_BNOT,
    SYMBOL_OP_NOT,
    SYMBOL_OP_INDEX,
    SYMBOL_OP_INDEX_SET,
    SYMBOL_OP_LSHIFT,
    SYMBOL_OP_RSHIFT,
    SYMBOL_OP_ADD_ASSIGN,
    SYMBOL_OP_SUB_ASSIGN,
    SYMBOL_OP_MUL_ASSIGN,
    SYMBOL_OP_DIV_ASSIGN,
    SYMBOL_OP_MOD_ASSIGN,
    SYMBOL_OP_AND_ASSIGN,
    SYMBOL_OP_OR_ASSIGN,
    SYMBOL_OP_XOR_ASSIGN,
    SYMBOL_OP_LSHIFT_ASSIGN,
    SYMBOL_OP_RSHIFT_ASSIGN,
    SYMBOL_OP_INC,
    SYMBOL_OP_DEC,

    // Coroutine/Task properties
    SYMBOL_DONE,
    SYMBOL_CANCELLED,
    SYMBOL_CANCEL,
    SYMBOL_RESULT,
    SYMBOL_ERROR,
    SYMBOL_MONITOR,
    SYMBOL_LINK,
    SYMBOL_UNLINK,

    // Channel methods
    SYMBOL_SEND,
    SYMBOL_RECV,
    SYMBOL_TRYSEND,
    SYMBOL_TRYRECV,
    SYMBOL_SENDTIMEOUT,
    SYMBOL_RECVTIMEOUT,
    SYMBOL_CLOSE,
    SYMBOL_IS_CLOSED,

    // BigInt methods
    SYMBOL_SIGN,
    SYMBOL_ISZERO,
    SYMBOL_ISNEGATIVE,
    SYMBOL_ISPOSITIVE,

    // Logger methods
    SYMBOL_LOG_DEBUG,
    SYMBOL_LOG_INFO,
    SYMBOL_LOG_WARN,
    SYMBOL_LOG_FATAL,
    SYMBOL_LOG_CHILD,

    // Array extended methods
    SYMBOL_FIND,
    SYMBOL_FINDINDEX,
    SYMBOL_EVERY,
    SYMBOL_SOME,
    SYMBOL_FILL,
    SYMBOL_SORT,
    SYMBOL_INCLUDES,
    SYMBOL_CAPACITY,

    // Regex methods
    SYMBOL_TEST,
    SYMBOL_FINDALL,
    SYMBOL_PATTERN,

    SYMBOL_BUILTIN_COUNT            // sentinel
};

// Semantic aliases for readability
#define SYMBOL_MAP_METHOD    SYMBOL_MAP
#define SYMBOL_CHARCODEAT    SYMBOL_CODEPOINT_AT
#define SYMBOL_LOG_ERROR     SYMBOL_ERROR

/* ========== Symbol Table Structure ========== */

typedef struct XrSymbolTable {
    XrHashMap *name_to_id;
    const char **id_to_name;
    int capacity;
    int count;
    int builtin_count;
} XrSymbolTable;

/* ========== Symbol Table API ========== */

XR_FUNC XrSymbolTable* xr_symbol_table_create(void);
XR_FUNC void xr_symbol_table_destroy(XrSymbolTable *table);
XR_FUNC bool xr_symbol_table_init_builtins(XrSymbolTable *table);
XR_FUNC SymbolId xr_symbol_register_in_table(XrSymbolTable *table, const char *name);
XR_FUNC SymbolId xr_symbol_lookup_in_table(XrSymbolTable *table, const char *name);
XR_FUNC const char* xr_symbol_get_name_in_table(XrSymbolTable *table, SymbolId id);

// Standalone builtin symbol lookup (no table instance needed).
// Returns SYMBOL_INVALID if name is not a builtin symbol.
XR_FUNC SymbolId xr_builtin_symbol_from_name(const char *name);

// Resolve symbol name by ID via isolate's symbol table.
// Takes void* to avoid circular header dependency with XrayIsolate.
XR_FUNC const char* xr_symbol_get_name_by_id(void *isolate, int symbol_id);

// Intern a name through the isolate's symbol table.
// Returns a stable pointer to the interned string, owned by the symbol
// table for the lifetime of the isolate. Callers must not free it.
// Returns NULL if isolate/name is NULL or allocation fails.
// Takes void* to avoid circular header dependency with XrayIsolate.
XR_FUNC const char* xr_symbol_intern(void *isolate, const char *name);

/* ========== Compile-time Checks ========== */

#if __STDC_VERSION__ >= 201112L
#include <assert.h>
_Static_assert(sizeof(SymbolId) == 4, "SymbolId must be 32-bit");
_Static_assert(SYMBOL_INVALID == 0, "SYMBOL_INVALID must be 0");
_Static_assert(SYMBOL_BUILTIN_COUNT <= 256, "Too many builtin symbols");
#endif

#endif // XSYMBOL_TABLE_H
