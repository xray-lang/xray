/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_builtins.c - Built-in type member definitions
 */

#include "xanalyzer_builtins.h"
#include "xanalyzer_native_types.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../base/xmalloc.h"
#include "../../../stdlib/prelude/prelude.h"
#include <string.h>

// Builtin type table populated at startup from .xr declarations.
static inline const XaBuiltinType *get_builtin_types(void) {
    if (!xa_native_types_ready())
        xa_native_types_init();
    return xa_native_get_builtin_types();
}

// XrType → XrTypeId
XrTypeId xr_type_to_builtin_id(XrType *type) {
    if (!type)
        return XR_TID_NULL;
    if (XR_TYPE_IS_INT(type))
        return XR_TID_INT;
    if (XR_TYPE_IS_FLOAT(type))
        return XR_TID_FLOAT;
    if (XR_TYPE_IS_STRING(type))
        return XR_TID_STRING;
    if (XR_TYPE_IS_BOOL(type))
        return XR_TID_BOOL;
    if (XR_TYPE_IS_ARRAY(type))
        return XR_TID_ARRAY;
    if (XR_TYPE_IS_MAP(type))
        return type->is_weak ? XR_TID_WEAKMAP : XR_TID_MAP;
    if (type->kind == XR_KIND_SET)
        return type->is_weak ? XR_TID_WEAKSET : XR_TID_SET;
    if (XR_TYPE_IS_JSON(type))
        return XR_TID_JSON;
    if (xr_type_is_named_class(type, "BigInt"))
        return XR_TID_BIGINT;
    if (xr_type_is_named_class(type, "StringBuilder"))
        return XR_TID_STRINGBUILDER;
    if (type->kind == XR_KIND_CHANNEL)
        return XR_TID_CHANNEL;
    if (type->kind == XR_KIND_ENUM)
        return XR_TID_ENUM_VALUE;
    if (xr_type_is_named_class(type, "Regex"))
        return XR_TID_REGEX;
    if (xr_type_is_named_class(type, "Exception"))
        return XR_TID_EXCEPTION;
    if (xr_type_is_named_class(type, "Task"))
        return XR_TID_COROUTINE;
    return XR_TID_NULL;
}

// Get built-in type info by XrType (O(1) via enum index)
const XaBuiltinType *xa_builtin_get_type_info(XrType *type) {
    XrTypeId id = xr_type_to_builtin_id(type);
    if (id == XR_TID_NULL)
        return NULL;
    const XaBuiltinType *bt = &get_builtin_types()[id];
    return bt->members ? bt : NULL;
}

// Get built-in type info by name (O(n) fallback for string-based lookup)
const XaBuiltinType *xa_builtin_get_by_name(const char *name) {
    if (!name)
        return NULL;
    const XaBuiltinType *table = get_builtin_types();
    for (int i = 0; i < XR_TID_COUNT; i++) {
        if (table[i].name && strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

// Create fake symbols for built-in members
XaSymbol **xa_builtin_get_members(XrType *type, int *count) {
    XR_DCHECK(count != NULL, "builtin_get_members: NULL count");
    *count = 0;

    const XaBuiltinType *bt = xa_builtin_get_type_info(type);
    if (!bt)
        return NULL;

    XaSymbol **symbols = xr_malloc(sizeof(XaSymbol *) * bt->member_count);
    if (!symbols)
        return NULL;

    for (int i = 0; i < bt->member_count; i++) {
        const XaBuiltinMember *m = &bt->members[i];
        XaSymbolKind kind = m->is_method ? XA_SYM_METHOD : XA_SYM_FIELD;
        XaSymbol *sym = xa_symbol_new(m->name, kind);
        sym->is_builtin = true;
        symbols[i] = sym;
    }

    *count = bt->member_count;
    return symbols;
}

// Get member signature for hover
const char *xa_builtin_get_member_signature(XrType *type, const char *member_name) {
    const XaBuiltinType *bt = xa_builtin_get_type_info(type);
    if (!bt || !member_name)
        return NULL;

    for (int i = 0; i < bt->member_count; i++) {
        if (strcmp(bt->members[i].name, member_name) == 0) {
            return bt->members[i].signature;
        }
    }
    return NULL;
}

// Get member documentation
const char *xa_builtin_get_member_doc(XrType *type, const char *member_name) {
    const XaBuiltinType *bt = xa_builtin_get_type_info(type);
    if (!bt || !member_name)
        return NULL;

    for (int i = 0; i < bt->member_count; i++) {
        if (strcmp(bt->members[i].name, member_name) == 0) {
            return bt->members[i].doc;
        }
    }
    return NULL;
}

// Check if member is a method
bool xa_builtin_is_method(XrType *type, const char *member_name) {
    XR_DCHECK(member_name != NULL, "builtin_is_method: NULL member_name");
    const XaBuiltinType *bt = xa_builtin_get_type_info(type);
    if (!bt || !member_name)
        return false;

    for (int i = 0; i < bt->member_count; i++) {
        if (strcmp(bt->members[i].name, member_name) == 0) {
            return bt->members[i].is_method;
        }
    }
    return false;
}

// Get method return type with generic substitution
XrType *xa_builtin_get_method_return_type(XrayIsolate *X, XrType *container_type,
                                          const char *method_name) {
    if (!container_type || !method_name)
        return NULL;

    // Convert method name to Symbol ID once
    SymbolId sym = xr_builtin_symbol_from_name(method_name);

    // Get element type for generic substitution
    XrType *elem_type = NULL;
    if (XR_TYPE_IS_ARRAY(container_type)) {
        elem_type = container_type->container.element_type;
    } else if (container_type->kind == XR_KIND_SET) {
        elem_type = container_type->container.element_type;
    } else if (container_type->kind == XR_KIND_CHANNEL) {
        elem_type = container_type->container.element_type;
    }

    // Array methods
    if (XR_TYPE_IS_ARRAY(container_type)) {
        switch (sym) {
            case SYMBOL_CONCAT:
                return xr_type_new_array(X, elem_type);
            case SYMBOL_EVERY:
                return xr_type_new_bool(NULL);
            case SYMBOL_FILTER:
            case SYMBOL_FILL:
                return xr_type_new_array(X, elem_type);
            case SYMBOL_FIND: {
                XrType *t = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
                if (t)
                    t->is_nullable = true;
                return t;
            }
            case SYMBOL_FINDINDEX:
                return xr_type_new_int(NULL);
            case SYMBOL_FOREACH:
                return xr_type_new_unit(NULL);
            case SYMBOL_INDEXOF:
                return xr_type_new_int(NULL);
            case SYMBOL_INCLUDES:
                return xr_type_new_bool(NULL);
            case SYMBOL_JOIN:
                return xr_type_new_string(NULL);
            case SYMBOL_MAP:
                return xr_type_new_array(X, xr_type_new_unknown(NULL));
            case SYMBOL_PUSH:
            case SYMBOL_UNSHIFT:
                return xr_type_new_unit(NULL);
            case SYMBOL_POP:
            case SYMBOL_SHIFT: {
                XrType *t = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
                if (t)
                    t->is_nullable = true;
                return t;
            }
            case SYMBOL_REVERSE:
            case SYMBOL_SLICE:
            case SYMBOL_SORT:
                return xr_type_new_array(X, elem_type);
            case SYMBOL_REDUCE:
                return xr_type_new_unknown(NULL);
            case SYMBOL_SOME:
                return xr_type_new_bool(NULL);
            default:
                break;
        }
    }

    // String methods
    if (XR_TYPE_IS_STRING(container_type)) {
        switch (sym) {
            case SYMBOL_CHARAT:
            case SYMBOL_CONCAT:
            case SYMBOL_SLICE:
            case SYMBOL_SUBSTRING:
            case SYMBOL_TOLOWERCASE:
            case SYMBOL_TOUPPERCASE:
            case SYMBOL_TRIM:
            case SYMBOL_TRIM_START:
            case SYMBOL_TRIM_END:
            case SYMBOL_REPLACE:
            case SYMBOL_REPLACEALL:
            case SYMBOL_REPEAT:
            case SYMBOL_PAD_START:
            case SYMBOL_PAD_END:
                return xr_type_new_string(NULL);
            case SYMBOL_CODEPOINT_AT:
            case SYMBOL_INDEXOF:
            case SYMBOL_LASTINDEXOF:
                return xr_type_new_int(NULL);
            case SYMBOL_CONTAINS:
            case SYMBOL_STARTSWITH:
            case SYMBOL_ENDSWITH:
                return xr_type_new_bool(NULL);
            case SYMBOL_SPLIT:
                return xr_type_new_array(X, xr_type_new_string(NULL));
            case SYMBOL_MATCH: {
                XrType *t = xr_type_new_array(X, xr_type_new_string(NULL));
                if (t)
                    t->is_nullable = true;
                return t;
            }
            default:
                break;
        }
    }

    // Map methods
    if (XR_TYPE_IS_MAP(container_type)) {
        XrType *key_type = container_type->map.key_type;
        XrType *val_type = container_type->map.value_type;

        switch (sym) {
            case SYMBOL_GET: {
                XrType *t = val_type ? xr_type_copy(X, val_type) : xr_type_new_unknown(NULL);
                if (t)
                    t->is_nullable = true;
                return t;
            }
            case SYMBOL_SET:
            case SYMBOL_CLEAR:
            case SYMBOL_FOREACH:
                return xr_type_new_unit(NULL);
            case SYMBOL_HAS:
            case SYMBOL_DELETE:
                return xr_type_new_bool(NULL);
            case SYMBOL_KEYS:
                return xr_type_new_array(X, key_type);
            case SYMBOL_VALUES:
                return xr_type_new_array(X, val_type);
            case SYMBOL_ENTRIES: {
                /* Map.entries(): Array<(K, V)> — each pair is a
                 * heterogeneous arity-2 tuple, so `for ((k, v) in
                 * m.entries())` destructures with the right typing. */
                XrType *pair_elems[2] = {key_type, val_type};
                XrType *pair = xr_type_new_tuple(X, pair_elems, 2);
                return xr_type_new_array(X, pair ? pair : xr_type_new_unknown(NULL));
            }
            case SYMBOL_MAP:
                return xr_type_new_map(X, key_type, xr_type_new_unknown(NULL));
            case SYMBOL_FILTER:
                return xr_type_new_map(X, key_type, val_type);
            case SYMBOL_REDUCE:
                return xr_type_new_unknown(NULL);
            default:
                break;
        }
    }

    // Set methods
    if (container_type->kind == XR_KIND_SET) {
        switch (sym) {
            case SYMBOL_ADD:
            case SYMBOL_CLEAR:
            case SYMBOL_FOREACH:
                return xr_type_new_unit(NULL);
            case SYMBOL_HAS:
            case SYMBOL_DELETE:
                return xr_type_new_bool(NULL);
            case SYMBOL_VALUES:
                return xr_type_new_array(X, elem_type);
            default:
                break;
        }
    }

    // Channel methods and properties
    if (container_type->kind == XR_KIND_CHANNEL) {
        switch (sym) {
            case SYMBOL_SEND:
            case SYMBOL_CLOSE:
                return xr_type_new_unit(NULL);
            case SYMBOL_RECV:
                return elem_type ? elem_type : xr_type_new_unknown(NULL);
            case SYMBOL_TRYSEND:
            case SYMBOL_IS_CLOSED:
                return xr_type_new_bool(NULL);
            case SYMBOL_TRYRECV: {
                XrType *t = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
                if (t)
                    t->is_nullable = true;
                return t;
            }
            case SYMBOL_LENGTH:
            case SYMBOL_CAPACITY:
                return xr_type_new_int(NULL);
            default:
                break;
        }
    }

    // int methods
    if (XR_TYPE_IS_INT(container_type)) {
        switch (sym) {
            case SYMBOL_ABS:
            case SYMBOL_MAX:
            case SYMBOL_MIN:
            case SYMBOL_FLOOR:
            case SYMBOL_CEIL:
            case SYMBOL_ROUND:
                return xr_type_new_int(NULL);
            case SYMBOL_TOSTRING:
            case SYMBOL_TOHEX:
                return xr_type_new_string(NULL);
            case SYMBOL_TOFLOAT:
                return xr_type_new_float(NULL);
            case SYMBOL_SQRT:
            case SYMBOL_POW:
                return xr_type_new_float(NULL);
            case SYMBOL_TOBIGINT:
                return xr_type_new_bigint(X);
            default:
                break;
        }
    }

    // float methods
    if (XR_TYPE_IS_FLOAT(container_type)) {
        switch (sym) {
            case SYMBOL_ABS:
            case SYMBOL_SQRT:
            case SYMBOL_POW:
                return xr_type_new_float(NULL);
            case SYMBOL_TOSTRING:
            case SYMBOL_TOFIXED:
                return xr_type_new_string(NULL);
            case SYMBOL_TOINT:
            case SYMBOL_FLOOR:
            case SYMBOL_CEIL:
            case SYMBOL_ROUND:
                return xr_type_new_int(NULL);
            default:
                break;
        }
    }

    // bool methods
    if (XR_TYPE_IS_BOOL(container_type)) {
        if (sym == SYMBOL_TOSTRING)
            return xr_type_new_string(NULL);
    }

    // BigInt methods
    if (xr_type_is_named_class(container_type, "BigInt")) {
        switch (sym) {
            case SYMBOL_ABS:
                return xr_type_new_bigint(X);
            case SYMBOL_TOSTRING:
                return xr_type_new_string(NULL);
            case SYMBOL_SIGN:
                return xr_type_new_int(NULL);
            case SYMBOL_ISZERO:
            case SYMBOL_ISNEGATIVE:
            case SYMBOL_ISPOSITIVE:
                return xr_type_new_bool(NULL);
            case SYMBOL_TOINT: {
                XrType *t = xr_type_new_int(NULL);
                if (t)
                    t->is_nullable = true;
                return t;
            }
            case SYMBOL_TOFLOAT:
                return xr_type_new_float(NULL);
            default:
                break;
        }
    }

    // Json methods
    if (XR_TYPE_IS_JSON(container_type)) {
        switch (sym) {
            case SYMBOL_KEYS:
                return xr_type_new_array(X, xr_type_new_string(NULL));
            case SYMBOL_VALUES:
                return xr_type_new_array(X, xr_type_new_json(NULL));
            case SYMBOL_ENTRIES: {
                /* Json.entries(): Array<(string, Json)> — every key
                 * in a Json object is a string at runtime; the value
                 * is the existential Json type. */
                XrType *pair_elems[2] = {xr_type_new_string(NULL), xr_type_new_json(NULL)};
                XrType *pair = xr_type_new_tuple(X, pair_elems, 2);
                return xr_type_new_array(X, pair ? pair : xr_type_new_unknown(NULL));
            }
            case SYMBOL_HAS:
            case SYMBOL_IS_EMPTY:
                return xr_type_new_bool(NULL);
            case SYMBOL_GET: {
                XrType *t = xr_type_new_json(NULL);
                if (t)
                    t->is_nullable = true;
                return t;
            }
            case SYMBOL_DELETE:
            case SYMBOL_CLEAR:
                return xr_type_new_unit(NULL);
            case SYMBOL_TOSTRING:
                return xr_type_new_string(NULL);
            default:
                break;
        }
    }

    // StringBuilder methods
    if (xr_type_is_named_class(container_type, "StringBuilder")) {
        switch (sym) {
            case SYMBOL_TOSTRING:
                return xr_type_new_string(NULL);
            case SYMBOL_CLEAR:
                return xr_type_new_stringbuilder(X);
            default:
                break;
        }
        // "append" is not a builtin symbol, handle separately
        if (strcmp(method_name, "append") == 0)
            return xr_type_new_stringbuilder(X);
    }

    return NULL;
}

// ============================================================================
// C Module type declarations (auto-generated from C source annotations)
// ============================================================================

#include "xanalyzer_builtins_generated.h"
#include "xanalyzer_xrd.h"

// Use generated module registry
static const XaBuiltinModule *builtin_modules = g_gen_builtin_modules;
static const int builtin_module_count = GEN_BUILTIN_MODULE_COUNT;

// Manually defined module signatures.
// Global objects: Coro, CoroPool, Reflect, Type
// Stdlib modules not yet in auto-generated registry: cluster

static const XaBuiltinMember g_rt_coro_functions[] = {
    {"stats", "(): Json", "Get coroutine statistics", true, true},
    {"list", "(limit?: int, state?: string): Array<Json>", "List coroutines", true, true},
    {"deadlocks", "(): Array<Json>", "Detect deadlocked coroutines", true, true},
    {"top", "(n: int, metric?: string): Array<Json>", "Top N coroutines by metric", true, true},
    {"groupBy", "(field: string): Json", "Group coroutines by field", true, true},
    {"setLocal", "(key: string, value: Json): void", "Set coroutine-local storage", true, true},
    {"getLocal", "(key: string): Json", "Get coroutine-local storage", true, true},
    {"setPriority", "(task: Json, priority: int): void", "Set coroutine priority", true, true},
    {"lockThread", "(): void", "Lock current thread", true, true},
    {"unlockThread", "(): void", "Unlock current thread", true, true},
    {"dump", "(limit?: int): void", "Dump coroutine state", true, true},
    {"stalled", "(timeout_ms?: int): Array<Json>", "Detect stalled coroutines", true, true},
    {"whereis", "(name: string): bool", "Check if named coroutine exists", true, true},
    {"monitor", "(name: string): Channel", "Monitor named coroutine, returns Channel", true, true},
    {"demonitor", "(ch: Channel): void", "Cancel coroutine monitor", true, true},
    {"self", "(): string?", "Get current coroutine name", true, true},
    {"kill", "(name: string, reason?: string): bool", "Kill named coroutine", true, true},
};
#define RT_CORO_FUNCTION_COUNT 17

static const XaBuiltinMember g_rt_coropool_functions[] = {
    {"submit", "(fn: function): Json", "Submit task to pool", true, false},
    {"close", "(): void", "Close the pool", true, false},
};
#define RT_COROPOOL_FUNCTION_COUNT 2

static const XaBuiltinMember g_rt_reflect_functions[] = {
    {"getType", "(obj: Json): Json", "Get type info of object", true, true},
    {"getTypeByName", "(name: string): Json", "Get type info by name", true, true},
    {"getAllTypes", "(): Array<Json>", "Get all registered types", true, true},
    {"isInstance", "(obj: Json, cls: Json): bool", "Check if obj is instance of cls", true, true},
    {"isInstanceOf", "(obj: Json, name: string): bool", "Check by class name", true, true},
    {"fieldCount", "(obj: Json): int", "Get field count of object", true, true},
    {"elementType", "(obj: Json): string", "Get element type of container", true, true},
    {"keyType", "(obj: Json): string", "Get key type of map", true, true},
    {"valueType", "(obj: Json): string", "Get value type of map", true, true},
    {"typeOf", "(obj: Json): string", "Get type name string", true, true},
};
#define RT_REFLECT_FUNCTION_COUNT 10

static const XaBuiltinMember g_rt_type_functions[] = {
    {"name", "(tid: int): string", "Get type name from TypeId", true, true},
};
#define RT_TYPE_FUNCTION_COUNT 1

static const XaBuiltinMember g_rt_cluster_functions[] = {
    {"start", "(config: Json): void", "Start cluster node", true, true},
    {"join", "(host: string, port: int): bool", "Join cluster", true, true},
    {"self", "(): string", "Get own node name", true, true},
    {"nodes", "(): Array<string>", "List cluster nodes", true, true},
    {"channel", "(name: string): Channel", "Get distributed channel", true, true},
    {"serve", "(name: string, handler: fn): void", "Register service handler", true, true},
    {"call", "(node: string, service: string, data: Json): Json", "Call remote service", true,
     true},
    {"reply", "(req: Json, result: Json): void", "Reply to service request", true, true},
    {"monitor", "(node: string): Channel", "Monitor node health", true, true},
    {"stop", "(): void", "Stop cluster node", true, true},
};
#define RT_CLUSTER_FUNCTION_COUNT 10

static const XaBuiltinModule g_rt_builtin_modules[] = {
    {"Coro", g_rt_coro_functions, RT_CORO_FUNCTION_COUNT, NULL, 0},
    {"CoroPool", g_rt_coropool_functions, RT_COROPOOL_FUNCTION_COUNT, NULL, 0},
    {"Reflect", g_rt_reflect_functions, RT_REFLECT_FUNCTION_COUNT, NULL, 0},
    {"Type", g_rt_type_functions, RT_TYPE_FUNCTION_COUNT, NULL, 0},
    {"cluster", g_rt_cluster_functions, RT_CLUSTER_FUNCTION_COUNT, NULL, 0},
};
#define RT_BUILTIN_MODULE_COUNT 5

// Script directory for .xrd search (set by analyzer or LSP)
static const char *g_script_dir = NULL;

void xa_builtin_set_script_dir(const char *dir) {
    g_script_dir = dir;
}

const XaBuiltinModule *xa_builtin_get_module_info(const char *module_name) {
    if (!module_name)
        return NULL;

    // 1. Search runtime modules (Coro, CoroPool, Reflect, Type)
    for (int i = 0; i < RT_BUILTIN_MODULE_COUNT; i++) {
        if (strcmp(g_rt_builtin_modules[i].name, module_name) == 0) {
            return &g_rt_builtin_modules[i];
        }
    }

    // 2. Search built-in (embedded) C modules
    for (int i = 0; i < builtin_module_count; i++) {
        if (strcmp(builtin_modules[i].name, module_name) == 0) {
            return &builtin_modules[i];
        }
    }

    // 3. Fall back to .xrd files for third-party modules
    return xa_xrd_find_module(module_name, g_script_dir);
}

const char *xa_builtin_get_module_func_signature(const char *module_name, const char *func_name) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info(module_name);
    if (!mod || !func_name)
        return NULL;
    for (int i = 0; i < mod->function_count; i++) {
        if (strcmp(mod->functions[i].name, func_name) == 0) {
            return mod->functions[i].signature;
        }
    }
    return NULL;
}

const char *xa_builtin_get_module_func_doc(const char *module_name, const char *func_name) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info(module_name);
    if (!mod || !func_name)
        return NULL;
    for (int i = 0; i < mod->function_count; i++) {
        if (strcmp(mod->functions[i].name, func_name) == 0) {
            return mod->functions[i].doc;
        }
    }
    return NULL;
}

const XaBuiltinHandle *xa_builtin_get_handle_type(const char *module_name,
                                                  const char *handle_name) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info(module_name);
    if (!mod || !handle_name)
        return NULL;
    for (int i = 0; i < mod->handle_count; i++) {
        if (strcmp(mod->handles[i].name, handle_name) == 0) {
            return &mod->handles[i];
        }
    }
    return NULL;
}

const XaBuiltinHandle *xa_builtin_find_handle_by_name(const char *handle_name) {
    if (!handle_name)
        return NULL;

    // Search builtin (embedded) C modules
    for (int i = 0; i < builtin_module_count; i++) {
        const XaBuiltinModule *mod = &builtin_modules[i];
        for (int j = 0; j < mod->handle_count; j++) {
            if (strcmp(mod->handles[j].name, handle_name) == 0)
                return &mod->handles[j];
        }
    }

    // Search dynamically loaded .xrd modules
    return xa_xrd_find_handle_by_name(handle_name);
}

// ============================================================================
// Generic API (used by both compiler and LSP)
// ============================================================================

int xa_builtin_get_members_for_type(XrType *type, const XaBuiltinMember **out_members) {
    const XaBuiltinType *info = xa_builtin_get_type_info(type);
    if (!info || !out_members)
        return 0;

    *out_members = info->members;
    return info->member_count;
}

const char *xa_builtin_get_type_name(XrType *type) {
    XrTypeId id = xr_type_to_builtin_id(type);
    if (id == XR_TID_NULL)
        return NULL;
    if (id == XR_TID_ARRAY)
        return TYPE_NAME_ARRAY;
    return get_builtin_types()[id].name;
}

// Parse a type string (e.g., "int", "string?", "Array<int>") to XrType
static XrType *parse_type_str(XrayIsolate *X, const char *s, size_t len);

// Helper for parse_type_str: when s starts with '(' return the byte index
// just past the matching ')' at depth 0; otherwise len. If the slice past
// that ')' (after optional whitespace) starts with "->", *out_has_arrow
// is set true and *out_arrow_pos is the byte index of '-' in "->".
static size_t find_balanced_close_paren(const char *s, size_t len, bool *out_has_arrow,
                                        size_t *out_arrow_pos) {
    if (out_has_arrow)
        *out_has_arrow = false;
    if (out_arrow_pos)
        *out_arrow_pos = 0;
    if (len < 2 || s[0] != '(')
        return len;

    int depth = 0;
    size_t i = 1;
    for (; i < len; i++) {
        if (s[i] == '(') {
            depth++;
        } else if (s[i] == ')') {
            if (depth == 0) {
                size_t close_after = i + 1;
                size_t p = close_after;
                while (p < len && s[p] == ' ')
                    p++;
                if (p + 1 < len && s[p] == '-' && s[p + 1] == '>') {
                    if (out_has_arrow)
                        *out_has_arrow = true;
                    if (out_arrow_pos)
                        *out_arrow_pos = p;
                }
                return close_after;
            }
            depth--;
        }
    }
    return len;
}

// Parse a "fn(p: T, ...): R" function type literal from a bounded slice.
// Mirrors xa_builtin_parse_full_signature but works on [s, s+len) instead
// of a NUL-terminated string, so it composes safely inside nested type
// expressions (e.g. the first parameter of Array<T>.reduce, which is
// itself a function type "fn(acc: U, item: T): U").
static XrType *parse_fn_type_str(XrayIsolate *X, const char *s, size_t len);

// Public wrapper with NUL-terminated string.
XrType *xa_builtin_parse_type_string(XrayIsolate *X, const char *s) {
    if (!s)
        return xr_type_new_unknown(NULL);
    return parse_type_str(X, s, strlen(s));
}

// Helper: skip leading whitespace.
static inline size_t parse_type_skip_ws(const char *s, size_t len, size_t i) {
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    return i;
}

// Helper: trim trailing whitespace from [start, end).
static inline size_t parse_type_trim_right(const char *s, size_t start, size_t end) {
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        end--;
    return end;
}

// Find a top-level pipe ('|') in s[0, len). Returns the index, or len
// if not found. "Top level" means not inside <...> generic args. The
// parser allows `int | string?` to mean `int | (string | null)`, so we
// only need to match a literal '|' between balanced angle brackets.
static size_t parse_type_find_top_pipe(const char *s, size_t len, size_t from) {
    int depth = 0;
    for (size_t i = from; i < len; i++) {
        char c = s[i];
        if (c == '<')
            depth++;
        else if (c == '>') {
            if (depth > 0)
                depth--;
        } else if (c == '|' && depth == 0) {
            return i;
        }
    }
    return len;
}

static XrType *parse_type_str(XrayIsolate *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xr_type_new_unknown(NULL);

    // Top-level union: T1 | T2 | ... . Mirrors the parser's union rule
    // in xr_parse_type_annotation, except we work on the cfunc-signature
    // string surface (no real tokens). Each member is parsed recursively
    // so that `int | string?` parses as `int | (string|null)` exactly
    // like xr_parse_type_annotation does.
    size_t first_pipe = parse_type_find_top_pipe(s, len, 0);
    if (first_pipe < len) {
        XrType *members[XR_UNION_MAX_MEMBERS];
        int count = 0;

        size_t start = 0;
        while (start <= len && count < XR_UNION_MAX_MEMBERS) {
            size_t pipe = parse_type_find_top_pipe(s, len, start);
            size_t mstart = parse_type_skip_ws(s, len, start);
            size_t mend = parse_type_trim_right(s, mstart, pipe);
            if (mend > mstart) {
                XrType *m = parse_type_str(X, s + mstart, mend - mstart);
                if (m)
                    members[count++] = m;
            }
            if (pipe >= len)
                break;
            start = pipe + 1;
        }

        if (count == 0)
            return xr_type_new_unknown(X);
        if (count == 1)
            return members[0];
        return xr_type_new_union(X, members, count);
    }

    // Strip trailing '?' for nullable check
    bool nullable = (s[len - 1] == '?');
    size_t base_len = nullable ? len - 1 : len;

    // Strip trailing '?' from optional params too
    // e.g., "int?" means int or null

    XrType *type = NULL;
    if (base_len == 3 && strncmp(s, TYPE_NAME_INT, 3) == 0) {
        type = xr_type_new_int(NULL);
    } else if (base_len == 5 && strncmp(s, TYPE_NAME_FLOAT, 5) == 0) {
        type = xr_type_new_float(NULL);
    } else if (base_len == 4 && strncmp(s, TYPE_NAME_BOOL, 4) == 0) {
        type = xr_type_new_bool(NULL);
    } else if (base_len == 6 && strncmp(s, TYPE_NAME_STRING, 6) == 0) {
        type = xr_type_new_string(NULL);
    } else if (base_len == 4 && strncmp(s, TYPE_NAME_VOID, 4) == 0) {
        type = xr_type_new_unit(NULL);
    } else if (base_len == 4 && strncmp(s, TYPE_NAME_JSON, 4) == 0) {
        type = xr_type_new_json(NULL);
    } else if (base_len == 7 && strncmp(s, TYPE_NAME_UNKNOWN, 7) == 0) {
        type = xr_type_new_unknown(NULL);
    } else if (base_len == 5 && strncmp(s, "Regex", 5) == 0) {
        type = xr_type_new_instance(X, NULL);
        type->instance.class_name = "Regex";
    } else if (base_len == 5 && strncmp(s, "Bytes", 5) == 0) {
        type = xr_type_new_bytes(X);
    } else if (base_len == 5 && strncmp(s, TYPE_NAME_NEVER, 5) == 0) {
        type = xr_type_new_never(NULL);
        // Native-width integer types
    } else if (base_len == 5 && strncmp(s, "uint8", 5) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_U8);
    } else if (base_len == 6 && strncmp(s, "uint16", 6) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_U16);
    } else if (base_len == 6 && strncmp(s, "uint32", 6) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_U32);
    } else if (base_len == 6 && strncmp(s, "uint64", 6) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_U64);
    } else if (base_len == 4 && strncmp(s, "int8", 4) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_I8);
    } else if (base_len == 5 && strncmp(s, "int16", 5) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_I16);
    } else if (base_len == 5 && strncmp(s, "int32", 5) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_I32);
    } else if (base_len == 5 && strncmp(s, "int64", 5) == 0) {
        type = xr_type_new_int_width(X, XR_NATIVE_I64);
    } else if (base_len == 7 && strncmp(s, "float32", 7) == 0) {
        type = xr_type_new_float_width(X, XR_NATIVE_F32);
    } else if (base_len == 7 && strncmp(s, "float64", 7) == 0) {
        type = xr_type_new_float_width(X, XR_NATIVE_F64);
        // Generic containers: recursively parse element types
    } else if (base_len >= 6 && strncmp(s, TYPE_NAME_ARRAY "<", 6) == 0) {
        // Array<ElemType>: parse inner type between '<' and last '>'
        const char *inner = s + 6;
        size_t inner_len = base_len - 7;  // strip "Array<" and ">"
        type = xr_type_new_array(X, parse_type_str(X, inner, inner_len));
    } else if (base_len >= 4 && strncmp(s, TYPE_NAME_MAP "<", 4) == 0) {
        // Map<K, V>: find comma separator at depth 0
        const char *inner = s + 4;
        size_t inner_len = base_len - 5;
        const char *comma = NULL;
        int d = 0;
        for (size_t i = 0; i < inner_len; i++) {
            if (inner[i] == '<')
                d++;
            else if (inner[i] == '>')
                d--;
            else if (inner[i] == ',' && d == 0) {
                comma = inner + i;
                break;
            }
        }
        if (comma) {
            size_t klen = comma - inner;
            const char *vstart = comma + 1;
            while (*vstart == ' ')
                vstart++;
            size_t vlen = inner_len - (vstart - inner);
            type =
                xr_type_new_map(X, parse_type_str(X, inner, klen), parse_type_str(X, vstart, vlen));
        } else {
            type = xr_type_new_map(X, xr_type_new_unknown(NULL), xr_type_new_unknown(NULL));
        }
    } else if (base_len >= 4 && strncmp(s, TYPE_NAME_SET "<", 4) == 0) {
        const char *inner = s + 4;
        size_t inner_len = base_len - 5;
        type = xr_type_new_set(X, parse_type_str(X, inner, inner_len));
    } else if (base_len >= 8 && strncmp(s, "Channel<", 8) == 0) {
        const char *inner = s + 8;
        size_t inner_len = base_len - 9;
        type = xr_type_new_channel(X, parse_type_str(X, inner, inner_len));
    } else if (base_len >= 3 && strncmp(s, "fn", 2) == 0 && (s[2] == '(' || s[2] == ' ')) {
        // fn(p: T, ...): R — legacy function type literal, still emitted by
        // some hand-authored XR_DEFINE_BUILTIN signatures.
        type = parse_fn_type_str(X, s, base_len);
    } else if (base_len >= 2 && s[0] == '(' &&
               /* (p: T, ...) -> R — current-syntax function type literal.
                * The helper peeks past the matching `)` for ` -> ` so a
                * leading `(` without a trailing arrow falls through to the
                * tuple branch (`(T, U, ...)`) below. */
               ({
                   bool _ha = false;
                   size_t _ap = 0;
                   (void) find_balanced_close_paren(s, base_len, &_ha, &_ap);
                   _ha;
               })) {
        bool has_arrow = false;
        size_t arrow_pos = 0;
        size_t close_after = find_balanced_close_paren(s, base_len, &has_arrow, &arrow_pos);
        (void) has_arrow; /* guaranteed true by the guard above */
        // Synthesise the legacy `fn(...): R` shape and reuse
        // parse_fn_type_str so all function-type parsing lives in one
        // place. close_after - 1 is the index of `)`.
        size_t close_paren = close_after - 1;
        size_t ret_start = arrow_pos + 2;
        while (ret_start < base_len && s[ret_start] == ' ')
            ret_start++;

        size_t params_len = close_paren - 1;
        size_t ret_len = base_len - ret_start;
        size_t synth_cap = 2 /*"fn"*/ + 1 /*"("*/ + params_len + 3 /*"): "*/ + ret_len + 1;
        char *synth = xr_malloc(synth_cap);
        if (synth) {
            size_t off = 0;
            synth[off++] = 'f';
            synth[off++] = 'n';
            synth[off++] = '(';
            if (params_len > 0) {
                memcpy(synth + off, s + 1, params_len);
                off += params_len;
            }
            synth[off++] = ')';
            synth[off++] = ':';
            synth[off++] = ' ';
            if (ret_len > 0) {
                memcpy(synth + off, s + ret_start, ret_len);
                off += ret_len;
            }
            synth[off] = '\0';
            type = parse_fn_type_str(X, synth, off);
            xr_free(synth);
        } else {
            type = xr_type_new_unknown(X);
        }
    } else if (base_len >= 2 && ((s[0] == '[' && s[base_len - 1] == ']') ||
                                 (s[0] == '(' && s[base_len - 1] == ')'))) {
        // Tuple type — both `(T, U, ...)` (real Xray syntax) and
        // `[T, U, ...]` are accepted. The fn / generic-container
        // branches above run first, so a leading `(` here is
        // unambiguous: it is a tuple type, not a parenthesised group
        // (signature strings have no grouping form).
        const char *inner = s + 1;
        size_t inner_len = base_len - 2;
        XrType *elems[XR_UNION_MAX_MEMBERS];
        int count = 0;
        size_t p = 0;
        while (p < inner_len && count < XR_UNION_MAX_MEMBERS) {
            while (p < inner_len && (inner[p] == ' ' || inner[p] == ','))
                p++;
            if (p >= inner_len)
                break;
            size_t e = p;
            int d = 0;
            while (e < inner_len) {
                char c = inner[e];
                if (c == '<' || c == '(' || c == '[')
                    d++;
                else if (c == '>' || c == ')' || c == ']')
                    d--;
                else if (c == ',' && d == 0)
                    break;
                e++;
            }
            elems[count++] = parse_type_str(X, inner + p, e - p);
            p = e;
        }
        if (count > 0)
            type = xr_type_new_tuple(X, elems, count);
        else
            type = xr_type_new_unit(X);
    } else if (base_len == 1 && s[0] >= 'A' && s[0] <= 'Z') {
        // Single uppercase letter: generic type parameter (T, K, V, etc.)
        char name[2] = {s[0], '\0'};
        type = xr_type_new_type_param(X, name, s[0] - 'A');
    } else {
        // Last resort: consult the prelude registry. SIMPLE entries
        // (BigInt, DateTime, Logger, NetConn, NetListener, Range,
        // StringBuilder) all surface here as named instances so that
        // typed cfunc signatures like "(): NetConn?" round-trip
        // through the analyzer. Generic / singleton kinds were already
        // handled by the dedicated branches above.
        if (X) {
            const XrPreludeSymbols *symbols = xr_prelude_get_symbols(X);
            if (symbols) {
                const XrPreludeTypeEntry *entry = xr_prelude_lookup_type(symbols, s, base_len);
                if (entry && entry->kind == (int) XR_PRELUDE_KIND_SIMPLE) {
                    type = xr_type_new_instance(X, NULL);
                    if (type)
                        type->instance.class_name = entry->name;
                }
            }
        }
        // Fall back to handle types from loaded modules (.xrd).
        // Creates an instance type whose class_name matches the handle
        // so that method resolution can find the handle's methods later.
        if (!type) {
            char name_buf[64];
            size_t copy_len = base_len < sizeof(name_buf) - 1 ? base_len : sizeof(name_buf) - 1;
            memcpy(name_buf, s, copy_len);
            name_buf[copy_len] = '\0';

            const XaBuiltinHandle *handle = xa_builtin_find_handle_by_name(name_buf);
            if (handle) {
                type = xr_type_new_instance(X, NULL);
                if (type)
                    type->instance.class_name = handle->name;
            }
        }
        if (!type)
            type = xr_type_new_unknown(X);
    }

    if (type && nullable) {
        type = xr_type_make_nullable(X, type);
    }
    return type;
}

// Parse a "fn(p: T, ...): R" function type literal from a bounded slice.
// Operates on [s, s+len) so it can be used recursively inside larger type
// expressions where the inner fn is not NUL-terminated.
static XrType *parse_fn_type_str(XrayIsolate *X, const char *s, size_t len) {
    XR_DCHECK(s != NULL, "parse_fn_type_str: NULL s");
    // Skip "fn" prefix and any spaces before '('.
    size_t i = 2;
    while (i < len && s[i] == ' ')
        i++;
    if (i >= len || s[i] != '(')
        return xr_type_new_unknown(X);
    size_t open = i;

    // Locate the matching ')' at depth 0.
    int depth = 0;
    size_t close = len;
    for (size_t j = open + 1; j < len; j++) {
        if (s[j] == '(') {
            depth++;
        } else if (s[j] == ')') {
            if (depth == 0) {
                close = j;
                break;
            }
            depth--;
        }
    }
    if (close == len)
        return xr_type_new_unknown(X);

    // Parse parameter list between (open, close).
    XrType *param_types[16];
    int param_count = 0;
    bool is_variadic = false;
    int min_params = 0;
    bool seen_optional = false;

    size_t p = open + 1;
    while (p < close && param_count < 16) {
        while (p < close && (s[p] == ' ' || s[p] == ','))
            p++;
        if (p >= close)
            break;

        // Rest parameter: "...name: T"
        if (p + 3 <= close && strncmp(s + p, "...", 3) == 0) {
            is_variadic = true;
            seen_optional = true;
            p += 3;
        }

        // Find ':' at top depth within this slice. The `->` arrow token must
        // be skipped so its `>` does not underflow depth.
        size_t colon = close;
        int d = 0;
        for (size_t c = p; c < close; c++) {
            if (c + 1 < close && s[c] == '-' && s[c + 1] == '>') {
                c++; /* skip the second char of "->" too */
                continue;
            }
            if (s[c] == '<' || s[c] == '(') {
                d++;
            } else if (s[c] == '>' || s[c] == ')') {
                d--;
            } else if (s[c] == ':' && d == 0) {
                colon = c;
                break;
            } else if (s[c] == ',' && d == 0) {
                break;
            }
        }

        if (colon < close) {
            bool is_optional = (colon > open + 1 && s[colon - 1] == '?');
            if (is_optional)
                seen_optional = true;

            size_t ts = colon + 1;
            while (ts < close && s[ts] == ' ')
                ts++;

            size_t te = ts;
            d = 0;
            while (te < close) {
                if (te + 1 < close && s[te] == '-' && s[te + 1] == '>') {
                    te += 2;
                    continue;
                }
                if (s[te] == '<' || s[te] == '(') {
                    d++;
                } else if (s[te] == '>' || s[te] == ')') {
                    d--;
                } else if (s[te] == ',' && d == 0) {
                    break;
                }
                te++;
            }

            param_types[param_count] = parse_type_str(X, s + ts, te - ts);
            if (!seen_optional)
                min_params = param_count + 1;
            param_count++;
            p = te;
        } else {
            while (p < close && s[p] != ',')
                p++;
            param_types[param_count] = xr_type_new_unknown(NULL);
            if (!seen_optional)
                min_params = param_count + 1;
            param_count++;
        }
    }

    // Parse return type: skip "): " after the closing paren.
    XrType *ret_type;
    size_t rt = close + 1;
    while (rt < len && s[rt] == ' ')
        rt++;
    if (rt < len && s[rt] == ':') {
        rt++;
        while (rt < len && s[rt] == ' ')
            rt++;
        ret_type = parse_type_str(X, s + rt, len - rt);
    } else {
        ret_type = xr_type_new_unit(NULL);
    }

    XrType **params = NULL;
    if (param_count > 0) {
        params = xr_malloc(sizeof(XrType *) * (size_t) param_count);
        XR_CHECK(params != NULL, "parse_fn_type_str: param array allocation failed");
        for (int k = 0; k < param_count; k++)
            params[k] = param_types[k];
    }
    XrType *fn_type = xr_type_new_function(X, params, param_count, ret_type, is_variadic);
    if (fn_type)
        fn_type->function.min_params = min_params;
    if (params)
        xr_free(params);
    return fn_type;
}

// Parse full function signature: "(param: type, param2: type): ReturnType"
// Returns a complete function type with parameter types
XrType *xa_builtin_parse_full_signature(XrayIsolate *X, const char *sig) {
    if (!sig)
        return xr_type_new_function(X, NULL, 0, xr_type_new_unknown(NULL), false);

    // Find parameter section: between '(' and matching ')'
    const char *open = strchr(sig, '(');
    if (!open)
        return xr_type_new_function(X, NULL, 0, xr_type_new_unknown(NULL), false);
    open++;

    // Find matching close paren at depth 0 (handles nested fn(...) types)
    const char *close = NULL;
    int depth = 0;
    for (const char *c = open; *c; c++) {
        if (*c == '(')
            depth++;
        else if (*c == ')') {
            if (depth == 0) {
                close = c;
                break;
            }
            depth--;
        }
    }
    if (!close || close <= open) {
        // Empty params "()"
        XrType *ret_type = xa_builtin_parse_return_type_from_sig(X, sig);
        return xr_type_new_function(X, NULL, 0, ret_type ? ret_type : xr_type_new_unit(NULL),
                                    false);
    }

    // Parse parameters: "param: type, param2: type"
    XrType *param_types[16];
    bool param_optional[16];
    int param_count = 0;
    int min_params = 0;
    bool is_variadic = false;
    bool seen_optional = false;

    const char *p = open;
    while (p < close && param_count < 16) {
        // Skip whitespace
        while (p < close && (*p == ' ' || *p == ','))
            p++;
        if (p >= close)
            break;

        // Check for rest parameter (rest params are always optional)
        if (strncmp(p, "...", 3) == 0) {
            is_variadic = true;
            seen_optional = true;
            p += 3;
        }

        // Find colon separator (track both <> and () depth for nested fn types).
        // Skip the `->` arrow token so its `>` does not underflow depth.
        const char *colon = NULL;
        int depth = 0;
        for (const char *c = p; c < close; c++) {
            if (c + 1 < close && c[0] == '-' && c[1] == '>') {
                c++; /* loop increments again, skipping both bytes */
                continue;
            }
            if (*c == '<' || *c == '(')
                depth++;
            else if (*c == '>' || *c == ')')
                depth--;
            else if (*c == ':' && depth == 0) {
                colon = c;
                break;
            } else if (*c == ',' && depth == 0)
                break;
        }

        if (colon && colon < close) {
            // Detect optional parameter: '?' immediately before ':'
            // e.g., "level?: int" or "compareFn?: fn(...)"
            bool is_optional = (colon > open && *(colon - 1) == '?');
            if (is_optional)
                seen_optional = true;
            param_optional[param_count] = is_optional;

            // Skip to type: after ": "
            const char *type_start = colon + 1;
            while (type_start < close && *type_start == ' ')
                type_start++;

            // Find end of type (next comma at depth 0 or close paren).
            // The `->` arrow token must not be misread as `>` closing a
            // generic — it would underflow depth and let inner commas
            // leak past the type boundary (e.g. the comma after
            // `(acc: U, item: T) -> U` separating the second `reduce`
            // parameter).
            const char *type_end = type_start;
            depth = 0;
            while (type_end < close) {
                if (type_end + 1 < close && type_end[0] == '-' && type_end[1] == '>') {
                    type_end += 2;
                    continue;
                }
                if (*type_end == '<' || *type_end == '(')
                    depth++;
                else if (*type_end == '>' || *type_end == ')')
                    depth--;
                else if (*type_end == ',' && depth == 0)
                    break;
                type_end++;
            }

            param_types[param_count] = parse_type_str(X, type_start, type_end - type_start);
            if (!seen_optional)
                min_params = param_count + 1;
            param_count++;
            p = type_end;
        } else {
            // No colon found, skip to next comma
            while (p < close && *p != ',')
                p++;
            param_optional[param_count] = false;
            param_types[param_count] = xr_type_new_unknown(NULL);
            if (!seen_optional)
                min_params = param_count + 1;
            param_count++;
        }
    }

    // Parse return type
    XrType *ret_type = xa_builtin_parse_return_type_from_sig(X, sig);
    if (!ret_type)
        ret_type = xr_type_new_unit(NULL);

    // Build function type
    XrType **params = NULL;
    if (param_count > 0) {
        params = xr_malloc(sizeof(XrType *) * param_count);
        for (int i = 0; i < param_count; i++) {
            params[i] = param_types[i];
        }
    }

    XrType *fn_type = xr_type_new_function(X, params, param_count, ret_type, is_variadic);
    if (fn_type)
        fn_type->function.min_params = min_params;
    if (params)
        xr_free(params);
    return fn_type;
}

// Parse return type from signature string. The supported separators are:
//   "(param: type) -> ReturnType"      (current arrow syntax)
//   "(param: type): ReturnType"        (legacy colon syntax, still present in
//                                       some hand-authored builtin tables)
// Returns an XrType based on the return type portion after the separator.
XrType *xa_builtin_parse_return_type_from_sig(XrayIsolate *X, const char *sig) {
    if (!sig)
        return NULL;

    // Find last separator. Both `): ` and `) -> ` are accepted; the rightmost
    // occurrence wins so nested function-type return values
    // (e.g. `(): fn(int): int` or `() -> (int) -> int`) parse correctly.
    const char *ret = NULL;
    const char *p = sig;
    while (*p) {
        if (p[0] == ')' && p[1] == ' ' && p[2] == '-' && p[3] == '>' && p[4] == ' ') {
            ret = p + 5;
            p += 5;
            continue;
        }
        if (p[0] == ')' && p[1] == ':' && p[2] == ' ') {
            ret = p + 3;
            p += 3;
            continue;
        }
        p++;
    }
    if (!ret || *ret == '\0')
        return xr_type_new_unit(NULL);

    return parse_type_str(X, ret, strlen(ret));
}
