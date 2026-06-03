/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsymbol_table.c - Symbol table system implementation
 *
 * KEY CONCEPT:
 *   Builtin symbol IDs are compile-time enum constants.
 *   Table-driven registration ensures enum order matches runtime IDs.
 *   Runtime symbols (user-defined) get IDs beyond SYMBOL_BUILTIN_COUNT.
 */

#include "xsymbol_table.h"
#include "../../base/xhashmap.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>
#include "../xisolate_api.h"

/*
 * Builtin symbol name table — order MUST match the enum in xsymbol_table.h.
 * Index 0 corresponds to SYMBOL_LENGTH (enum value 1), etc.
 * SYMBOL_LOG_ERROR is an alias for SYMBOL_ERROR (same string "error"),
 * so "error" appears only once.
 */
static const char *xr_builtin_symbol_names[] = {
    // Common methods
    "length",
    "isEmpty",
    "has",
    "get",
    "set",
    "delete",
    "clear",
    "keys",
    "values",
    "entries",
    // Functional methods
    "hasValue",
    "forEach",
    "map",
    "filter",
    "reduce",
    // String methods
    "slice",
    "charAt",
    "substring",
    "indexOf",
    "contains",
    "startsWith",
    "endsWith",
    "toLowerCase",
    "toUpperCase",
    "trim",
    "split",
    "replace",
    "replaceAll",
    "repeat",
    "concat",
    "byteAt",
    "reverseBytes",
    "translate",
    "translateBytes",
    "trimStart",
    "trimEnd",
    "padStart",
    "padEnd",
    "lastIndexOf",
    "toInt",
    "toFloat",
    "isLetter",
    "isNumber",
    "isAlphanumeric",
    "isWhitespace",
    "ord",
    "match",
    // Array methods
    "push",
    "pop",
    "shift",
    "unshift",
    "join",
    "reverse",
    // Iterator methods
    "iterator",
    "entriesIterator",
    "hasNext",
    "next",
    // Number methods
    "floor",
    "ceil",
    "round",
    "abs",
    "sqrt",
    "pow",
    "toFixed",
    "toBigInt",
    "max",
    "min",
    "toHex",
    // UTF-8 methods
    "charLength",
    "byteLength",
    "codePointAt",
    "fromCodePoint",
    "chars",
    // Set methods
    "add",
    "union",
    "intersection",
    "difference",
    "symmetricDifference",
    "toArray",
    "isSubset",
    "isSuperset",
    // Enum/type methods
    "name",
    "value",
    "ordinal",
    "toString",
    "memberCount",
    "getMember",
    // DateTime methods
    "format",
    "year",
    "month",
    "day",
    "hour",
    "minute",
    "second",
    "weekday",
    "timestamp",
    "toUTC",
    "toLocal",
    "millisecond",
    "isBefore",
    "isAfter",
    "equals",
    "isLeapYear",
    "daysInMonth",
    "toISOString",
    "yearday",
    // Operator overload symbols
    "+",
    "-",
    "*",
    "/",
    "%",
    "==",
    "!=",
    "<",
    "<=",
    ">",
    ">=",
    "&",
    "|",
    "^",
    "~",
    "!",
    "[]",
    "[]=",
    "<<",
    ">>",
    "+=",
    "-=",
    "*=",
    "/=",
    "%=",
    "&=",
    "|=",
    "^=",
    "<<=",
    ">>=",
    "++",
    "--",
    // Coroutine/Task properties
    "done",
    "cancelled",
    "cancel",
    "result",
    "error",
    "monitor",
    "link",
    "unlink",
    // Channel methods
    "send",
    "recv",
    "trySend",
    "tryRecv",
    "sendTimeout",
    "recvTimeout",
    "close",
    "isClosed",
    // BigInt methods
    "sign",
    "isZero",
    "isNegative",
    "isPositive",
    // Logger methods
    "debug",
    "info",
    "warn",
    "fatal",
    "child",
    // Array extended methods
    "find",
    "findIndex",
    "every",
    "some",
    "fill",
    "sort",
    "includes",
    "capacity",
    // Regex methods
    "test",
    "findAll",
    "pattern",
    // Json type-checking instance methods
    "isNull",
    "isInt",
    "isFloat",
    "isString",
    "isBool",
    "isArray",
    "isObject",
};

#define BUILTIN_NAME_COUNT                                                                         \
    (int) (sizeof(xr_builtin_symbol_names) / sizeof(xr_builtin_symbol_names[0]))

/* ========== Internal Helper Functions ========== */

static bool expand_capacity(XrSymbolTable *table) {
    XR_DCHECK(table != NULL, "expand_capacity: NULL table");
    XR_DCHECK(table->capacity > 0, "expand_capacity: zero capacity");
    int new_capacity = table->capacity * 2;
    size_t new_size = sizeof(const char *) * (size_t) new_capacity;

    const char **new_array = (const char **) xr_realloc(table->id_to_name, new_size);
    if (!new_array)
        return false;

    memset(new_array + table->capacity, 0,
           sizeof(const char *) * (size_t) (new_capacity - table->capacity));
    table->id_to_name = new_array;
    table->capacity = new_capacity;
    return true;
}

/* ========== Symbol Table API Implementation ========== */

XrSymbolTable *xr_symbol_table_create(void) {
    XrSymbolTable *table = (XrSymbolTable *) xr_malloc(sizeof(XrSymbolTable));
    if (!table)
        return NULL;

    table->name_to_id = xr_hashmap_new();
    if (!table->name_to_id) {
        xr_free(table);
        return NULL;
    }

    table->capacity = 256;
    table->count = 0;
    table->builtin_count = 0;

    table->id_to_name = (const char **) xr_malloc(sizeof(const char *) * (size_t) table->capacity);
    if (!table->id_to_name) {
        xr_hashmap_free(table->name_to_id);
        xr_free(table);
        return NULL;
    }
    memset(table->id_to_name, 0, sizeof(const char *) * (size_t) table->capacity);

    return table;
}

void xr_symbol_table_destroy(XrSymbolTable *table) {
    if (!table)
        return;

    // Builtin names are NOT freed (they point to string literals).
    // Only free user-registered symbol names.
    for (int i = table->builtin_count; i < table->count; i++) {
        if (table->id_to_name[i])
            xr_free((void *) table->id_to_name[i]);
    }
    xr_free(table->id_to_name);
    xr_hashmap_free(table->name_to_id);
    xr_free(table);
}

bool xr_symbol_table_init_builtins(XrSymbolTable *table) {
    if (!table)
        return false;

    XR_CHECK(BUILTIN_NAME_COUNT == SYMBOL_BUILTIN_COUNT - 1,
             "symbol table: builtin name count mismatch");

    for (int i = 0; i < BUILTIN_NAME_COUNT; i++) {
        const char *name = xr_builtin_symbol_names[i];
        SymbolId expected_id = i + 1;

        // Store directly — no strdup needed for string literals
        table->id_to_name[table->count] = name;
        xr_hashmap_set(table->name_to_id, name, (void *) (intptr_t) expected_id);
        table->count++;

        XR_DCHECK(table->count == (int) expected_id, "symbol table: builtin id ordering mismatch");
    }

    table->builtin_count = table->count;
    return true;
}

SymbolId xr_symbol_register_in_table(XrSymbolTable *table, const char *name) {
    if (!table || !name)
        return SYMBOL_INVALID;

    SymbolId existing = xr_symbol_lookup_in_table(table, name);
    if (existing != SYMBOL_INVALID)
        return existing;

    if (table->count >= table->capacity) {
        if (!expand_capacity(table))
            return SYMBOL_INVALID;
    }

    SymbolId new_id = table->count + 1;
    XR_DCHECK(new_id > 0, "symbol_register: new_id overflow");

    size_t name_len = strlen(name);
    char *name_copy = (char *) xr_malloc(name_len + 1);
    if (!name_copy)
        return SYMBOL_INVALID;
    memcpy(name_copy, name, name_len + 1);

    table->id_to_name[table->count] = name_copy;
    xr_hashmap_set(table->name_to_id, name_copy, (void *) (intptr_t) new_id);
    table->count++;
    XR_DCHECK(table->count <= table->capacity, "symbol_register: count > capacity");

    return new_id;
}

SymbolId xr_symbol_lookup_in_table(XrSymbolTable *table, const char *name) {
    if (!table || !name)
        return SYMBOL_INVALID;

    void *value = xr_hashmap_get(table->name_to_id, name);
    if (!value)
        return SYMBOL_INVALID;

    return (SymbolId) (intptr_t) value;
}

const char *xr_symbol_get_name_in_table(XrSymbolTable *table, SymbolId id) {
    if (!table)
        return NULL;
    int index = id - 1;
    if (index < 0 || index >= table->count)
        return NULL;
    return table->id_to_name[index];
}

/* ========== Builtin Symbol Fast Lookup (O(1) hash, const table) ========== */

// Compile-time generated open-addressing hash table.
// Regenerate: python3 scripts/gen_builtin_hash.py > src/runtime/symbol/xbuiltin_hash.inc
#include "xbuiltin_hash.inc"

static uint32_t builtin_hash_fn(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t) *s;
        h *= 16777619u;
    }
    return h;
}

SymbolId xr_builtin_symbol_from_name(const char *name) {
    if (!name)
        return SYMBOL_INVALID;

    uint32_t slot = builtin_hash_fn(name) & (BUILTIN_HASH_SIZE - 1);
    while (xr_builtin_hash_table[slot].name != NULL) {
        if (strcmp(xr_builtin_hash_table[slot].name, name) == 0) {
            return xr_builtin_hash_table[slot].id;
        }
        slot = (slot + 1) & (BUILTIN_HASH_SIZE - 1);
    }
    return SYMBOL_INVALID;
}

const char *xr_symbol_get_name_by_id(void *isolate, int symbol_id) {
    if (!isolate)
        return NULL;
    XrSymbolTable *table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    return xr_symbol_get_name_in_table(table, (SymbolId) symbol_id);
}

const char *xr_symbol_intern(void *isolate, const char *name) {
    if (!isolate || !name)
        return NULL;
    XrSymbolTable *table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    if (!table)
        return NULL;
    SymbolId id = xr_symbol_register_in_table(table, name);
    if (id == SYMBOL_INVALID)
        return NULL;
    return xr_symbol_get_name_in_table(table, id);
}
