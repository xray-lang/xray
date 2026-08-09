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
    if (XR_TYPE_IS_RUNE(type))
        return XR_TID_RUNE;
    if (XR_TYPE_IS_BOOL(type))
        return XR_TID_BOOL;
    if (XR_TYPE_IS_ARRAY(type))
        return XR_TID_ARRAY;
    if (XR_TYPE_IS_MAP(type))
        return XR_TID_MAP;
    if (type->kind == XR_KIND_SET)
        return XR_TID_SET;
    if (XR_TYPE_IS_JSON(type))
        return XR_TID_OBJECT;
    if (XR_TYPE_IS_STRUCT_OBJECT(type))
        return XR_TID_OBJECT;
    /* Every named check below must exclude a user class that reuses the
     * builtin's name. This function is the single gate for builtin identity —
     * it decides the member table (xa_builtin_get_type_info), the native
     * capability flags (xanalyzer_capability.c), and LSP completion — so a
     * name-only match would let `class Semaphore { }` inherit the builtin's
     * methods and its INTERIOR_MUTABLE | SYNC_SHAREABLE grant. */
    if (xr_type_is_builtin_named_class(type, "BigInt"))
        return XR_TID_BIGINT;
    if (xr_type_is_builtin_named_class(type, "StringBuilder"))
        return XR_TID_STRINGBUILDER;
    if (type->kind == XR_KIND_CHANNEL)
        return XR_TID_CHANNEL;
    if (xr_type_is_builtin_named_class(type, "Regex"))
        return XR_TID_REGEX;
    if (xr_type_is_builtin_named_class(type, "PanicInfo"))
        return XR_TID_PANIC_INFO;
    if (xr_type_is_builtin_named_class(type, "Task"))
        return XR_TID_COROUTINE;
    if (xr_type_is_builtin_named_class(type, "Atomic"))
        return XR_TID_ATOMIC;
    if (xr_type_is_builtin_named_class(type, "WorkQueue"))
        return XR_TID_WORKQUEUE;
    if (xr_type_is_builtin_named_class(type, "ResultGroup"))
        return XR_TID_RESULTGROUP;
    if (xr_type_is_builtin_named_class(type, "CountdownLatch"))
        return XR_TID_COUNTDOWNLATCH;
    if (xr_type_is_builtin_named_class(type, "Semaphore"))
        return XR_TID_SEMAPHORE;
    if (xr_type_is_builtin_named_class(type, "EventCount"))
        return XR_TID_EVENTCOUNT;
    if (xr_type_is_builtin_named_class(type, "Thread"))
        return XR_TID_THREAD;
    if (xr_type_is_builtin_named_class(type, "Buffer"))
        return XR_TID_BUFFER;
    return XR_TID_NULL;
}

// Get built-in type info by XrType (O(1) via enum index)
const XaBuiltinType *xa_builtin_get_type_info(XrType *type) {
    if (!type)
        return NULL;
    if (XR_TYPE_IS_JSON(type))
        return xa_native_get_compiler_builtin_type("JSON");
    if (xr_type_is_builtin_named_class(type, "CoroLocal"))
        return xa_native_get_compiler_builtin_type("CoroLocal");
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
    const XaBuiltinType *compiler_type = xa_native_get_compiler_builtin_type(name);
    if (compiler_type)
        return compiler_type;
    const XaBuiltinType *table = get_builtin_types();
    for (int i = 0; i < XR_TID_COUNT; i++) {
        if (table[i].name && strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

static bool xa_builtin_member_available_for_type(XrType *type, const char *member_name) {
    if (!type || !member_name)
        return false;
    if (xr_type_is_named_class(type, "WorkQueue") && strcmp(member_name, "pushRange") == 0) {
        XrType *elem = (type->instance.type_arg_count > 0 && type->instance.type_args)
                           ? type->instance.type_args[0]
                           : NULL;
        return elem && elem->kind == XR_KIND_INT;
    }
    return true;
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
        if (!xa_builtin_member_available_for_type(type, m->name))
            continue;
        XaSymbolKind kind = m->is_method ? XA_SYM_METHOD : XA_SYM_FIELD;
        XaSymbol *sym = xa_symbol_new(m->name, kind);
        sym->is_builtin = true;
        symbols[*count] = sym;
        (*count)++;
    }

    return symbols;
}

static const XaBuiltinMember *xa_builtin_find_instance_member(XrType *type,
                                                              const char *member_name) {
    const XaBuiltinType *bt = xa_builtin_get_type_info(type);
    if (!bt || !member_name)
        return NULL;
    if (!xa_builtin_member_available_for_type(type, member_name))
        return NULL;
    for (int i = 0; i < bt->member_count; i++) {
        const XaBuiltinMember *m = &bt->members[i];
        if (!m->is_static && strcmp(m->name, member_name) == 0)
            return m;
    }
    return NULL;
}

static const XaBuiltinMember *xa_builtin_find_named_type_member(const XaBuiltinType *bt,
                                                                const char *member_name,
                                                                bool is_static) {
    if (!bt || !member_name)
        return NULL;
    for (int i = 0; i < bt->member_count; i++) {
        const XaBuiltinMember *m = &bt->members[i];
        if (m->is_static == is_static && strcmp(m->name, member_name) == 0)
            return m;
    }
    return NULL;
}

// Get member signature for instance access and hover
const char *xa_builtin_get_member_signature(XrType *type, const char *member_name) {
    const XaBuiltinMember *m = xa_builtin_find_instance_member(type, member_name);
    return m ? m->signature : NULL;
}

const XaEffectContract *
xa_builtin_get_type_member_effect_contract(XrType *type, const char *member_name, bool is_static) {
    const XaBuiltinType *bt = xa_builtin_get_type_info(type);
    if (!bt || !member_name)
        return NULL;
    if (!is_static && !xa_builtin_member_available_for_type(type, member_name))
        return NULL;
    const XaBuiltinMember *m = xa_builtin_find_named_type_member(bt, member_name, is_static);
    return m ? &m->effect_contract : NULL;
}

XaAllocationContractKind xa_builtin_get_type_member_allocation_contract(XrType *type,
                                                                        const char *member_name,
                                                                        bool is_static) {
    const XaBuiltinType *bt = xa_builtin_get_type_info(type);
    if (!bt || !member_name)
        return XA_ALLOCATION_CONTRACT_MISSING;
    if (!is_static && !xa_builtin_member_available_for_type(type, member_name))
        return XA_ALLOCATION_CONTRACT_MISSING;
    const XaBuiltinMember *m = xa_builtin_find_named_type_member(bt, member_name, is_static);
    return m ? m->allocation_contract : XA_ALLOCATION_CONTRACT_MISSING;
}

const XaEffectContract *xa_builtin_get_named_type_member_effect_contract(const char *type_name,
                                                                         const char *member_name,
                                                                         bool is_static) {
    const XaBuiltinType *bt = xa_builtin_get_by_name(type_name);
    const XaBuiltinMember *m = xa_builtin_find_named_type_member(bt, member_name, is_static);
    return m ? &m->effect_contract : NULL;
}

XaAllocationContractKind
xa_builtin_get_named_type_member_allocation_contract(const char *type_name, const char *member_name,
                                                     bool is_static) {
    const XaBuiltinType *bt = xa_builtin_get_by_name(type_name);
    const XaBuiltinMember *m = xa_builtin_find_named_type_member(bt, member_name, is_static);
    if (m)
        return m->allocation_contract;

    /* Compiler-defined nominal value types do not occupy a native runtime
     * type-table slot, but their allocation behavior is still an explicit
     * language contract. Keep those contracts next to the builtin registry
     * instead of teaching the allocation analysis method-name heuristics. */
    typedef struct XaCompilerTypeAllocationContract {
        const char *type_name;
        const char *member_name;
        bool is_static;
        XaAllocationContractKind allocation;
    } XaCompilerTypeAllocationContract;
    static const XaCompilerTypeAllocationContract compiler_type_contracts[] = {
        {"Range", "count", false, XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"Range", "contains", false, XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"Range", "toArray", false, XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"Range", "toString", false, XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"Range", "iterator", false, XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"<enum>", "toString", false, XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"<null>", "toString", false, XA_ALLOCATION_CONTRACT_MAY_HEAP},
    };
    for (size_t i = 0; i < sizeof(compiler_type_contracts) / sizeof(compiler_type_contracts[0]);
         i++) {
        const XaCompilerTypeAllocationContract *contract = &compiler_type_contracts[i];
        if (contract->is_static == is_static && strcmp(contract->type_name, type_name) == 0 &&
            strcmp(contract->member_name, member_name) == 0)
            return contract->allocation;
    }
    return XA_ALLOCATION_CONTRACT_MISSING;
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
    const XaBuiltinMember *m = xa_builtin_find_instance_member(type, member_name);
    return m ? m->is_method : false;
}

bool xa_builtin_member_mutates_receiver(XrType *type, const char *member_name) {
    if (!type || !member_name)
        return false;
    const XaBuiltinMember *member = xa_builtin_find_instance_member(type, member_name);
    return member && member->is_method && member->mutates_receiver;
}

bool xa_builtin_member_returns_receiver(XrType *type, const char *member_name) {
    if (!type || !member_name)
        return false;
    const XaBuiltinMember *member = xa_builtin_find_instance_member(type, member_name);
    return member && member->is_method && member->return_ownership == XA_BUILTIN_RETURN_RECEIVER;
}

/* R2-2 stopgap: see xanalyzer_builtins.h. The runtime bindings for the
 * checked/saturating/overflows families evaluate at int64 width (VM native
 * cfuncs and AOT xrt_method dispatch both see a widened i64 value), so on a
 * fixed-width receiver they would silently use the WRONG overflow boundary
 * (int32.checkedAdd reports no overflow at 2^31) and even invert the safety
 * contract the methods exist for. Until a width-carrying lowering exists,
 * fail the compile with an actionable message instead.
 *
 * Nullable fixed-width receivers are NOT special-cased: `int32?` follows the
 * language-wide nullable-widening rule (its value semantics are plain `int`,
 * e.g. `(a!) + 1` computes at int64), so the int64 method semantics are
 * consistent there. */
bool xa_builtin_int_overflow_method_unsupported(XrType *receiver, const char *method_name,
                                                char *msg, size_t msg_cap) {
    if (!receiver || !method_name || receiver->kind != XR_KIND_INT || receiver->is_nullable)
        return false;
    if (receiver->scalar_rep == XR_NATIVE_I64)
        return false;  // plain int / explicit int64: int64 semantics are exact

    static const char *const blocked[] = {
        "checkedAdd",    "checkedSub",   "checkedMul",   "saturatingAdd", "saturatingSub",
        "saturatingMul", "addOverflows", "subOverflows", "mulOverflows",
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (strcmp(method_name, blocked[i]) == 0) {
            if (msg && msg_cap > 0)
                snprintf(msg, msg_cap,
                         "'%s' is not supported on fixed-width integer receivers yet: the runtime "
                         "computes it at int64 width, which would silently use the wrong overflow "
                         "boundary; convert the receiver with int(...) first if int64 semantics "
                         "are intended, or use wrappingAdd/wrappingSub/wrappingMul (width-exact)",
                         method_name);
            return true;
        }
    }
    return false;
}

// Get method return type with generic substitution
XrType *xa_builtin_get_method_return_type(XrVMRuntime *X, XrType *container_type,
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
    } else if ((xr_type_is_builtin_named_class(container_type, "WorkQueue") ||
                xr_type_is_builtin_named_class(container_type, "Atomic")) &&
               container_type->instance.type_arg_count > 0) {
        elem_type = container_type->instance.type_args[0];
    }

    if (xr_type_is_builtin_named_class(container_type, "WorkQueue")) {
        if (sym == SYMBOL_PUSH)
            return xr_type_new_bool(NULL);
        if (sym == SYMBOL_POP) {
            XrType *t = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
            if (t)
                t->is_nullable = true;
            return t;
        }
        if (strcmp(method_name, "tryPop") == 0) {
            XrType *item = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
            if (item)
                item->is_nullable = true;
            XrType *elems[2] = {item, xr_type_new_bool(NULL)};
            return xr_type_new_tuple(X, elems, 2);
        }
        if (sym == SYMBOL_CLOSE)
            return xr_type_new_unit(NULL);
    }

    if (xr_type_is_builtin_named_class(container_type, "ResultGroup")) {
        if (strcmp(method_name, "add") == 0)
            return xr_type_new_bool(NULL);
        if (strcmp(method_name, "reset") == 0)
            return xr_type_new_bool(NULL);
        if (strcmp(method_name, "flush") == 0 || sym == SYMBOL_CLOSE)
            return xr_type_new_unit(NULL);
        if (strcmp(method_name, "recv") == 0) {
            return xr_type_make_nullable(X, xr_type_new_int(NULL));
        }
        if (strcmp(method_name, "tryRecv") == 0) {
            XrType *item = xr_type_make_nullable(X, xr_type_new_int(NULL));
            XrType *elems[2] = {item, xr_type_new_bool(NULL)};
            return xr_type_new_tuple(X, elems, 2);
        }
    }

    if (xr_type_is_builtin_named_class(container_type, "Semaphore")) {
        if (strcmp(method_name, "release") == 0)
            return xr_type_new_int(NULL);
        if (strcmp(method_name, "tryAcquire") == 0 || strcmp(method_name, "acquire") == 0)
            return xr_type_new_bool(NULL);
        if (sym == SYMBOL_CLOSE)
            return xr_type_new_unit(NULL);
    }

    if (xr_type_is_builtin_named_class(container_type, "EventCount")) {
        if (strcmp(method_name, "advance") == 0 || strcmp(method_name, "wait") == 0)
            return xr_type_new_int(NULL);
        if (sym == SYMBOL_CLOSE)
            return xr_type_new_unit(NULL);
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
            case SYMBOL_CONTAINS:
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
            case SYMBOL_SLICE:
            case SYMBOL_REPLACE:
            case SYMBOL_REPLACEALL:
            case SYMBOL_REPEAT:
                return xr_type_new_string(NULL);
            case SYMBOL_INDEXOF:
            case SYMBOL_LASTINDEXOF:
                return xr_type_new_int(NULL);
            case SYMBOL_CONTAINS:
            case SYMBOL_STARTSWITH:
            case SYMBOL_ENDSWITH:
                return xr_type_new_bool(NULL);
            case SYMBOL_SPLIT:
                return xr_type_new_array(X, xr_type_new_string(NULL));
            default:
                break;
        }
    }

    // char methods
    if (XR_TYPE_IS_RUNE(container_type)) {
        switch (sym) {
            case SYMBOL_TOSTRING:
                return xr_type_new_string(NULL);
            case SYMBOL_TO_UINT32:
                return xr_type_new_int(NULL);
            case SYMBOL_IS_LETTER:
            case SYMBOL_IS_NUMBER:
            case SYMBOL_IS_ALNUM:
            case SYMBOL_IS_WHITESPACE:
                return xr_type_new_bool(NULL);
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
            case SYMBOL_CONTAINS_KEY:
            case SYMBOL_CONTAINS_VALUE:
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
            case SYMBOL_CONTAINS:
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
            case SYMBOL_RECV: {
                XrType *t = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
                XrType *args[1] = {t};
                return xr_type_new_generic_instance(X, "Recv", NULL, args, 1);
            }
            case SYMBOL_IS_CLOSED:
                return xr_type_new_bool(NULL);
            case SYMBOL_TRYSEND:
                return xr_type_new_enum(X, "SendResult");
            case SYMBOL_SENDTIMEOUT:
                return xr_type_new_enum(X, "SendResult");
            case SYMBOL_TRYRECV: {
                XrType *t = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
                XrType *args[1] = {t};
                return xr_type_new_generic_instance(X, "Recv", NULL, args, 1);
            }
            case SYMBOL_RECVTIMEOUT: {
                XrType *t = elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
                XrType *args[1] = {t};
                return xr_type_new_generic_instance(X, "Recv", NULL, args, 1);
            }
            case SYMBOL_RECVOR:
                return elem_type ? xr_type_copy(X, elem_type) : xr_type_new_unknown(NULL);
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
            case SYMBOL_SATURATING_ADD:
            case SYMBOL_SATURATING_SUB:
            case SYMBOL_SATURATING_MUL:
            case SYMBOL_WRAPPING_ADD:
            case SYMBOL_WRAPPING_SUB:
            case SYMBOL_WRAPPING_MUL:
                return xr_type_new_int(NULL);
            case SYMBOL_CHECKED_ADD:
            case SYMBOL_CHECKED_SUB:
            case SYMBOL_CHECKED_MUL: {
                return xr_type_make_nullable(X, xr_type_new_int(NULL));
            }
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
            case SYMBOL_ISNAN:
                return xr_type_new_bool(NULL);
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
    if (xr_type_is_builtin_named_class(container_type, "BigInt")) {
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
                return xr_type_make_nullable(X, xr_type_new_int(NULL));
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
                /* JSON.entries(): Array<(string, Json)> — every key
                 * in a Json object is a string at runtime; the value
                 * is the existential Json type. */
                XrType *pair_elems[2] = {xr_type_new_string(NULL), xr_type_new_json(NULL)};
                XrType *pair = xr_type_new_tuple(X, pair_elems, 2);
                return xr_type_new_array(X, pair ? pair : xr_type_new_unknown(NULL));
            }
            case SYMBOL_CONTAINS_KEY:
                return xr_type_new_bool(NULL);
            case SYMBOL_GET: {
                return xr_type_make_nullable(X, xr_type_new_json(NULL));
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

    // Atomic methods — return types depend on inner type T
    if (xr_type_is_builtin_named_class(container_type, "Atomic")) {
        XrType *inner = NULL;
        if (container_type->instance.type_arg_count > 0)
            inner = container_type->instance.type_args[0];
        if (!inner)
            inner = xr_type_new_unknown(NULL);

        /* load() -> T, swap(v) -> T, fetchAdd(v) -> T, fetchSub(v) -> T */
        if (strcmp(method_name, "load") == 0 || strcmp(method_name, "swap") == 0 ||
            strcmp(method_name, "fetchAdd") == 0 || strcmp(method_name, "fetchSub") == 0) {
            return inner;
        }
        /* store(v), add(v), sub(v) -> () */
        if (strcmp(method_name, "store") == 0 || strcmp(method_name, "add") == 0 ||
            strcmp(method_name, "sub") == 0) {
            return xr_type_new_unit(NULL);
        }
        /* compareExchange(e, d) -> (T, bool) */
        if (strcmp(method_name, "compareExchange") == 0) {
            XrType *pair_elems[2] = {inner, xr_type_new_bool(NULL)};
            return xr_type_new_tuple(X, pair_elems, 2);
        }
        /* toggle() -> bool */
        if (strcmp(method_name, "toggle") == 0) {
            return xr_type_new_bool(NULL);
        }
        if (sym == SYMBOL_TOSTRING)
            return xr_type_new_string(NULL);
    }

    // StringBuilder methods
    if (xr_type_is_builtin_named_class(container_type, "StringBuilder")) {
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

/* Generated rows that do not yet declare an effect contract intentionally
 * zero-initialize the trailing field to XA_EFFECT_CONTRACT_MISSING. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "xanalyzer_builtins_generated.h"
#include "xanalyzer_xrd.h"

// Use generated module registry
static const XaBuiltinModule *builtin_modules = g_gen_builtin_modules;
static const int builtin_module_count = GEN_BUILTIN_MODULE_COUNT;

// Manually defined module signatures for VM-intrinsic global objects.
// These use special opcodes (OP_CORO_CTRL etc.), not module XRS_EXPORT.

static const XaBuiltinMember g_rt_coro_functions[] = {
    {"yield", "(): ()", "Cooperative CPU yield (Gosched)", true, true, false, false, true},
    {"stats", "(): CoroStats", "Get coroutine statistics", true, true, false, false, false},
    {"list", "(limit?: int, state?: CoroState): Array<CoroInfo>", "List coroutines", true, true,
     false, false, false},
    {"deadlocks", "(): Array<CoroDeadlock>", "Detect deadlocked coroutines", true, true, false,
     false, false},
    {"top", "(n: int, metric?: CoroMetric): Array<CoroInfo>", "Top N coroutines by metric", true,
     true, false, false, false},
    {"groupBy", "(field: CoroGroupKey): Map<string, int>", "Group coroutines by field", true, true,
     false, false, false},
    {"Local", "<T>(): CoroLocal<T>", "Create a typed coroutine-local slot", true, true, false,
     false, false},
    {"lockThread", "(): ()", "Lock current thread", true, true, false, false, false},
    {"unlockThread", "(): ()", "Unlock current thread", true, true, false, false, false},
    {"dump", "(limit?: int): ()", "Dump coroutine state", true, true, false, false, false},
    {"stalled", "(timeout_ms?: int): Array<CoroInfo>", "Detect stalled coroutines", true, true,
     false, false, false},
    {"whereis", "(name: string): bool", "Check if named coroutine exists", true, true, false, false,
     false},
    {"monitor", "(name: string): Channel<string>", "Monitor named coroutine, returns Channel", true,
     true, false, false, false},
    {"demonitor", "(ch: Channel<string>): ()", "Cancel coroutine monitor", true, true, false, false,
     false},
    {"self", "(): string?", "Get current coroutine name", true, true, false, false, false},
    {"kill", "(name: string, reason?: string): bool", "Kill named coroutine", true, true, false,
     false, false},
};
#define RT_CORO_FUNCTION_COUNT                                                                     \
    ((int) (sizeof(g_rt_coro_functions) / sizeof(g_rt_coro_functions[0])))

static const XaBuiltinMember g_rt_coropool_functions[] = {
    {"submit", "(fn: fn(): T): Task<T>", "Submit a typed task", true, false, false, false, false},
};
#define RT_COROPOOL_FUNCTION_COUNT                                                                 \
    ((int) (sizeof(g_rt_coropool_functions) / sizeof(g_rt_coropool_functions[0])))

static const XaBuiltinModule g_rt_builtin_modules[] = {
    {"Coro", g_rt_coro_functions, RT_CORO_FUNCTION_COUNT, NULL, 0, g_gen_Coro_object_shapes,
     GEN_CORO_OBJECT_SHAPE_COUNT, g_gen_Coro_enums, GEN_CORO_ENUM_COUNT},
    {"CoroPool", g_rt_coropool_functions, RT_COROPOOL_FUNCTION_COUNT, NULL, 0, NULL, 0, NULL, 0},
};
#define RT_BUILTIN_MODULE_COUNT 2

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Script directory for .xrd search (set by analyzer or LSP)
static const char *g_script_dir = NULL;

void xa_builtin_set_script_dir(const char *dir) {
    g_script_dir = dir;
}

XR_FUNCDEF int xa_builtin_get_module_count(void) {
    return RT_BUILTIN_MODULE_COUNT + builtin_module_count;
}

XR_FUNCDEF const XaBuiltinModule *xa_builtin_get_module_at(int index) {
    if (index < 0)
        return NULL;
    if (index < RT_BUILTIN_MODULE_COUNT)
        return &g_rt_builtin_modules[index];
    index -= RT_BUILTIN_MODULE_COUNT;
    if (index < builtin_module_count)
        return &builtin_modules[index];
    return NULL;
}

const XaBuiltinModule *xa_builtin_get_module_info(const char *module_name) {
    if (!module_name)
        return NULL;

    // 1. Search runtime intrinsic modules (Coro, CoroPool)
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

static bool xa_builtin_member_public(const XaBuiltinMember *member) {
    return member && !member->is_internal;
}

static const XaBuiltinMember *
xa_builtin_find_module_function(const char *module_name, const char *func_name, bool public_only) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info(module_name);
    if (!mod || !func_name)
        return NULL;
    for (int i = 0; i < mod->function_count; i++) {
        const XaBuiltinMember *member = &mod->functions[i];
        if (strcmp(member->name, func_name) == 0 &&
            (!public_only || xa_builtin_member_public(member)))
            return member;
    }
    return NULL;
}

const char *xa_builtin_get_module_func_signature(const char *module_name, const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, true);
    return member ? member->signature : NULL;
}

const char *xa_builtin_get_module_func_abi_signature(const char *module_name,
                                                     const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, false);
    return member ? member->signature : NULL;
}

const char *xa_builtin_get_module_func_doc(const char *module_name, const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, true);
    return member ? member->doc : NULL;
}

const XaEffectContract *xa_builtin_get_module_func_effect_contract(const char *module_name,
                                                                   const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, true);
    return member ? &member->effect_contract : NULL;
}

const XaEffectContract *xa_builtin_get_module_func_abi_effect_contract(const char *module_name,
                                                                       const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, false);
    return member ? &member->effect_contract : NULL;
}

XaAllocationContractKind xa_builtin_get_module_func_allocation_contract(const char *module_name,
                                                                        const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, true);
    return member ? member->allocation_contract : XA_ALLOCATION_CONTRACT_MISSING;
}

XaBuiltinReturnOwnership xa_builtin_get_module_func_return_ownership(const char *module_name,
                                                                     const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, true);
    return member ? member->return_ownership : XA_BUILTIN_RETURN_UNKNOWN;
}

bool xa_builtin_module_func_is_yieldable(const char *module_name, const char *func_name) {
    const XaBuiltinMember *member = xa_builtin_find_module_function(module_name, func_name, false);
    return member && member->is_method && member->is_yieldable;
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

const XaBuiltinObjectShape *xa_builtin_get_object_shape(const char *module_name,
                                                        const char *object_shape_name) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info(module_name);
    if (!mod || !object_shape_name)
        return NULL;
    for (int i = 0; i < mod->object_shape_count; i++) {
        if (mod->object_shapes[i].name &&
            strcmp(mod->object_shapes[i].name, object_shape_name) == 0)
            return &mod->object_shapes[i];
    }
    return NULL;
}

const XaBuiltinObjectShape *xa_builtin_find_object_shape_by_name(const char *object_shape_name) {
    if (!object_shape_name)
        return NULL;
    for (int i = 0; i < xa_builtin_get_module_count(); i++) {
        const XaBuiltinModule *mod = xa_builtin_get_module_at(i);
        if (!mod)
            continue;
        for (int j = 0; j < mod->object_shape_count; j++) {
            if (mod->object_shapes[j].name &&
                strcmp(mod->object_shapes[j].name, object_shape_name) == 0)
                return &mod->object_shapes[j];
        }
    }
    return NULL;
}

const XaBuiltinEnum *xa_builtin_get_enum_type(const char *module_name, const char *enum_name) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info(module_name);
    if (!mod || !enum_name)
        return NULL;
    for (int i = 0; i < mod->enum_count; i++) {
        if (mod->enums[i].name && strcmp(mod->enums[i].name, enum_name) == 0)
            return &mod->enums[i];
    }
    return NULL;
}

const XaBuiltinEnum *xa_builtin_find_enum_by_name(const char *enum_name) {
    if (!enum_name)
        return NULL;
    for (int i = 0; i < xa_builtin_get_module_count(); i++) {
        const XaBuiltinModule *mod = xa_builtin_get_module_at(i);
        if (!mod)
            continue;
        for (int j = 0; j < mod->enum_count; j++) {
            if (mod->enums[j].name && strcmp(mod->enums[j].name, enum_name) == 0)
                return &mod->enums[j];
        }
    }
    return NULL;
}

XrType *xa_builtin_object_shape_decl_type(XrVMRuntime *X,
                                          const XaBuiltinObjectShape *object_shape) {
    if (!object_shape || object_shape->field_count < 0 ||
        (object_shape->field_count > 0 && !object_shape->fields))
        return xr_type_new_error(X);
    int count = object_shape->field_count;
    const char **names = count > 0 ? xr_malloc(sizeof(*names) * (size_t) count) : NULL;
    XrType **types = count > 0 ? xr_malloc(sizeof(*types) * (size_t) count) : NULL;
    if (count > 0 && (!names || !types)) {
        xr_free(names);
        xr_free(types);
        return xr_type_new_error(X);
    }
    for (int i = 0; i < count; i++) {
        names[i] = object_shape->fields[i].name;
        types[i] = xa_builtin_parse_type_string(X, object_shape->fields[i].type_str);
    }
    XrType *type = xr_type_new_struct_object_with_fields(X, names, types, count);
    xr_free(names);
    xr_free(types);
    return type ? type : xr_type_new_error(X);
}

static const char *xa_builtin_enum_nominal_owner(const XaBuiltinEnum *enum_decl) {
    if (!enum_decl)
        return NULL;
    for (int i = 0; i < builtin_module_count; i++) {
        const XaBuiltinModule *module = &builtin_modules[i];
        for (int j = 0; j < module->enum_count; j++) {
            if (&module->enums[j] == enum_decl)
                return module->name;
        }
    }
    return NULL;
}

XrType *xa_builtin_enum_decl_type(XrVMRuntime *X, const XaBuiltinEnum *enum_decl,
                                  XaEnumInfo **out_info) {
    if (out_info)
        *out_info = NULL;
    if (!enum_decl || !enum_decl->name || enum_decl->variant_count <= 0 || !enum_decl->variants)
        return xr_type_new_error(X);

    const char *nominal_owner = xa_builtin_enum_nominal_owner(enum_decl);
    if (!nominal_owner)
        return xr_type_new_error(X);
    XaEnumInfo *info =
        xa_enum_info_new(nominal_owner, enum_decl->name, (uint32_t) enum_decl->variant_count);
    if (!info)
        return xr_type_new_error(X);
    for (int i = 0; i < enum_decl->variant_count; i++) {
        const XaBuiltinEnumVariant *src = &enum_decl->variants[i];
        XaEnumVariantInfo *dst = &info->variants[i];
        dst->name = src->name;
        dst->payload_count = (uint16_t) src->payload_count;
        if (src->payload_count > 0) {
            dst->payload_types = xr_calloc((size_t) src->payload_count, sizeof(XrType *));
            if (!dst->payload_types) {
                xa_enum_info_free(info);
                return xr_type_new_error(X);
            }
            for (int p = 0; p < src->payload_count; p++) {
                dst->payload_types[p] = xa_builtin_parse_type_string(X, src->payload_type_strs[p]);
            }
        }
    }
    if (!xa_enum_info_finalize_layout(info)) {
        xa_enum_info_free(info);
        return xr_type_new_error(X);
    }
    if (info->layout && enum_decl->layout_id != 0)
        info->layout->layout_id = enum_decl->layout_id;
    XrType *type = xr_type_new_enum(X, enum_decl->name);
    if (!type) {
        xa_enum_info_free(info);
        return xr_type_new_error(X);
    }
    if (out_info) {
        type->enum_type.layout = info->layout;
        type->enum_type.layout_id = info->layout ? info->layout->layout_id : 0;
        *out_info = info;
    } else {
        xa_enum_info_free(info);
    }
    return type;
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

const XaEffectContract *xa_builtin_get_handle_method_effect_contract(const char *handle_name,
                                                                     const char *method_name) {
    const XaBuiltinHandle *handle = xa_builtin_find_handle_by_name(handle_name);
    if (!handle || !method_name)
        return NULL;
    for (int i = 0; i < handle->method_count; i++) {
        const XaBuiltinMember *method = &handle->methods[i];
        if (method->is_method && method->name && strcmp(method->name, method_name) == 0)
            return &method->effect_contract;
    }
    return NULL;
}

XaAllocationContractKind xa_builtin_get_handle_method_allocation_contract(const char *handle_name,
                                                                          const char *method_name) {
    const XaBuiltinHandle *handle = xa_builtin_find_handle_by_name(handle_name);
    if (!handle || !method_name)
        return XA_ALLOCATION_CONTRACT_MISSING;
    for (int i = 0; i < handle->method_count; i++) {
        const XaBuiltinMember *method = &handle->methods[i];
        if (method->is_method && method->name && strcmp(method->name, method_name) == 0)
            return method->allocation_contract;
    }
    return XA_ALLOCATION_CONTRACT_MISSING;
}

/* Every name the language provides without an import. A user declaration may
 * not take one of these: the name would resolve to the user's type in some
 * positions and to the builtin in others, which is not a shadowing rule anyone
 * can reason about. Statics were the clearest case -- a user class declaring
 * `static withCapacity` still ran Array's, and `JSON.parse` on a user Json
 * reached the runtime before failing -- but the type-annotation path was no
 * better, rendering both types as `Array<int>` in a "not assignable to itself"
 * diagnostic. The list is the same registry the resolver and the LSP read, so
 * it cannot drift from what the language actually provides. */
static const char *const g_reserved_builtin_names[] = {
#define XR_BUILTIN_PRELUDE_TYPE(name, arity, native_type, prelude_kind) name,
#define XR_BUILTIN_TYPE(name, arity) name,
#define XR_BUILTIN_ENUM(name, arity, vm_slot, variants) name,
#define XR_BUILTIN_IFACE(name, arity) name,
#include "../../../stdlib/prelude/builtin_symbols.def"
};

bool xa_builtin_name_is_reserved(const char *name) {
    if (!name)
        return false;
    for (size_t i = 0; i < sizeof(g_reserved_builtin_names) / sizeof(g_reserved_builtin_names[0]);
         i++) {
        if (strcmp(g_reserved_builtin_names[i], name) == 0)
            return true;
    }
    return false;
}

const char *xa_builtin_find_handle_module(const char *handle_name) {
    if (!handle_name)
        return NULL;
    for (int i = 0; i < builtin_module_count; i++) {
        const XaBuiltinModule *mod = &builtin_modules[i];
        for (int j = 0; j < mod->handle_count; j++) {
            if (strcmp(mod->handles[j].name, handle_name) == 0)
                return mod->name;
        }
    }
    return NULL;
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
    if (XR_TYPE_IS_JSON(type))
        return TYPE_NAME_JSON;
    XrTypeId id = xr_type_to_builtin_id(type);
    if (id == XR_TID_NULL)
        return NULL;
    if (id == XR_TID_ARRAY)
        return TYPE_NAME_ARRAY;
    return get_builtin_types()[id].name;
}

// Parse a type string (e.g., "int", "string?", "Array<int>") to XrType
static XrType *parse_type_str(XrVMRuntime *X, const char *s, size_t len);

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

static inline bool has_arrow_after_paren(const char *s, size_t len) {
    bool ha = false;
    (void) find_balanced_close_paren(s, len, &ha, NULL);
    return ha;
}

// Parse a "fn(p: T, ...): R" function type literal from a bounded slice.
// Mirrors xa_builtin_parse_full_signature but works on [s, s+len) instead
// of a NUL-terminated string, so it composes safely inside nested type
// expressions (e.g. the first parameter of Array<T>.reduce, which is
// itself a function type "fn(acc: U, item: T): U").
static XrType *parse_fn_type_str(XrVMRuntime *X, const char *s, size_t len);

// Public wrapper with NUL-terminated string.
XrType *xa_builtin_parse_type_string(XrVMRuntime *X, const char *s) {
    if (!s)
        return xr_type_new_error(X);
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

static XrType *parse_type_str(XrVMRuntime *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xr_type_new_error(X);

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
            return xr_type_new_error(X);
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
    XrSourceTypeSpelling scalar_source = xr_source_type_spelling_lookup(s, base_len);
    if (scalar_source != XR_SOURCE_TYPE_NONE) {
        uint8_t scalar_rep = xr_source_type_scalar_rep(scalar_source);
        type = xr_scalar_rep_is_float(scalar_rep) ? xr_type_new_float_width(X, scalar_rep)
                                                  : xr_type_new_int_width(X, scalar_rep);
    }
    if (!type && base_len == 4 && strncmp(s, TYPE_NAME_BOOL, 4) == 0) {
        type = xr_type_new_bool(NULL);
    } else if (base_len == 6 && strncmp(s, TYPE_NAME_STRING, 6) == 0) {
        type = xr_type_new_string(NULL);
    } else if (base_len == 4 && strncmp(s, TYPE_NAME_RUNE, 4) == 0) {
        type = xr_type_new_rune(NULL);
    } else if (base_len == 4 && strncmp(s, TYPE_NAME_VOID, 4) == 0) {
        type = xr_type_new_unit(NULL);
    } else if (base_len == strlen(TYPE_NAME_JSON) &&
               strncmp(s, TYPE_NAME_JSON, strlen(TYPE_NAME_JSON)) == 0) {
        type = xr_type_new_json(NULL);
    } else if (base_len == strlen("JSON.Object") &&
               strncmp(s, "JSON.Object", strlen("JSON.Object")) == 0) {
        type = xr_type_new_map(X, xr_type_new_string(NULL), xr_type_new_json(X));
    } else if (base_len == strlen("JSON.PathSegment") &&
               strncmp(s, "JSON.PathSegment", strlen("JSON.PathSegment")) == 0) {
        XrType *members[2] = {xr_type_new_string(NULL), xr_type_new_int(NULL)};
        type = xr_type_new_union(X, members, 2);
    } else if (base_len == strlen("JSON.Path") &&
               strncmp(s, "JSON.Path", strlen("JSON.Path")) == 0) {
        XrType *members[2] = {xr_type_new_string(NULL), xr_type_new_int(NULL)};
        type = xr_type_new_array(X, xr_type_new_union(X, members, 2));
    } else if (base_len == strlen("JSON.UnknownFields") &&
               strncmp(s, "JSON.UnknownFields", strlen("JSON.UnknownFields")) == 0) {
        type = xr_type_new_enum(X, "JSON.UnknownFields");
    } else if (base_len == 7 && strncmp(s, TYPE_NAME_UNKNOWN, 7) == 0) {
        type = xr_type_new_error(X);
    } else if (base_len == 8 && strncmp(s, "Ordering", 8) == 0) {
        type = xr_type_new_enum(X, "Ordering");
    } else if (base_len == 6 && strncmp(s, "Endian", 6) == 0) {
        type = xr_type_new_enum(X, "Endian");
    } else if (base_len == 4 && strncmp(s, "Recv", 4) == 0) {
        type = xr_type_new_enum(X, "Recv");
    } else if (base_len == 10 && strncmp(s, "SendResult", 10) == 0) {
        type = xr_type_new_enum(X, "SendResult");
    } else if (base_len == 10 && strncmp(s, "TaskResult", 10) == 0) {
        type = xr_type_new_enum(X, "TaskResult");
    } else if (base_len == 10 && strncmp(s, "TaskStatus", 10) == 0) {
        type = xr_type_new_enum(X, "TaskStatus");
    } else if (base_len == 9 && strncmp(s, "Utf8Error", 9) == 0) {
        type = xr_type_new_enum(X, "Utf8Error");
    } else if (base_len == 16 && strncmp(s, "StringSliceError", 16) == 0) {
        type = xr_type_new_enum(X, "StringSliceError");
    } else if (base_len == 16 && strncmp(s, "CompressionError", 16) == 0) {
        type = xr_type_new_enum(X, "CompressionError");
    } else if (base_len == 11 && strncmp(s, "CryptoError", 11) == 0) {
        type = xr_type_new_enum(X, "CryptoError");
    } else if (base_len == 5 && strncmp(s, "Regex", 5) == 0) {
        type = xr_type_new_instance(X, NULL);
        type->instance.class_name = "Regex";
    } else if (base_len == 10 && strncmp(s, "RegexMatch", 10) == 0) {
        type = xr_type_new_instance(X, NULL);
        type->instance.class_name = "RegexMatch";
    } else if (base_len == strlen(TYPE_NAME_BUFFER) &&
               strncmp(s, TYPE_NAME_BUFFER, strlen(TYPE_NAME_BUFFER)) == 0) {
        type = xr_type_new_instance(X, NULL);
        type->instance.class_name = TYPE_NAME_BUFFER;
    } else if (base_len == 5 && strncmp(s, TYPE_NAME_NEVER, 5) == 0) {
        type = xr_type_new_never(NULL);
    } else if (base_len == 4 && strncmp(s, TYPE_NAME_NULL, 4) == 0) {
        type = xr_type_new_null(NULL);
        // Generic containers: recursively parse element types
    } else if (base_len >= 6 && strncmp(s, TYPE_NAME_ARRAY "<", 6) == 0) {
        // Array<ElemType>: parse inner type between '<' and last '>'
        const char *inner = s + 6;
        size_t inner_len = base_len - 7;  // strip "Array<" and ">"
        type = xr_type_new_array(X, parse_type_str(X, inner, inner_len));
    } else if (base_len >= strlen(TYPE_NAME_SLICE) + 2 &&
               strncmp(s, TYPE_NAME_SLICE "<", strlen(TYPE_NAME_SLICE) + 1) == 0) {
        const char *inner = s + strlen(TYPE_NAME_SLICE) + 1;
        size_t inner_len = base_len - strlen(TYPE_NAME_SLICE) - 2;
        type = xr_type_new_slice(X, parse_type_str(X, inner, inner_len));
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
            type = xr_type_new_error(X);
        }
    } else if (base_len >= 4 && strncmp(s, TYPE_NAME_SET "<", 4) == 0) {
        const char *inner = s + 4;
        size_t inner_len = base_len - 5;
        type = xr_type_new_set(X, parse_type_str(X, inner, inner_len));
    } else if (base_len >= 8 && strncmp(s, "Channel<", 8) == 0) {
        const char *inner = s + 8;
        size_t inner_len = base_len - 9;
        type = xr_type_new_channel(X, parse_type_str(X, inner, inner_len));
    } else if (base_len >= 4 && s[base_len - 1] == '>') {
        const char *lt = NULL;
        int depth = 0;
        for (size_t i = 0; i < base_len; i++) {
            if (s[i] == '<') {
                if (depth == 0) {
                    lt = s + i;
                    break;
                }
                depth++;
            }
        }
        if (lt && lt > s) {
            size_t name_len = (size_t) (lt - s);
            char name_buf[64];
            size_t copy_len = name_len < sizeof(name_buf) - 1 ? name_len : sizeof(name_buf) - 1;
            memcpy(name_buf, s, copy_len);
            name_buf[copy_len] = '\0';

            const char *inner = lt + 1;
            size_t inner_len = (size_t) ((s + base_len - 1) - inner);
            XrType *args[16];
            int argc = 0;
            size_t p = 0;
            while (p < inner_len && argc < 16) {
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
                args[argc++] = parse_type_str(X, inner + p, e - p);
                p = e;
            }

            if (strcmp(name_buf, "Task") == 0 && argc >= 1) {
                type = xr_type_new_task(X, args[0]);
            } else if (strcmp(name_buf, "JSON.WithRest") == 0 && argc == 1) {
                const char *field_names[2] = {"rest", "value"};
                XrType *field_types[2] = {
                    xr_type_new_map(X, xr_type_new_string(NULL), xr_type_new_json(X)), args[0]};
                type = xr_type_new_struct_object_with_fields(X, field_names, field_types, 2);
                if (type)
                    type->object.type_name = "JSON.WithRest";
            } else if (strcmp(name_buf, "Ptr") == 0 && argc >= 1) {
                // Raw pointers must round-trip as XR_KIND_POINTER (not a generic
                // named instance) so stdlib-function pointer params compare equal
                // to extern-derived Ptr/MutPtr args (mirrors xtype_ref_resolve).
                type = xr_type_new_pointer(X, args[0], false);
            } else if (strcmp(name_buf, "MutPtr") == 0 && argc >= 1) {
                type = xr_type_new_pointer(X, args[0], true);
            } else if (argc > 0) {
                type = xr_type_new_generic_instance(X, name_buf, NULL, args, argc);
            }
        }
    } else if (base_len >= 3 && strncmp(s, "fn", 2) == 0 && (s[2] == '(' || s[2] == ' ')) {
        // fn(p: T, ...): R — legacy function type literal accepted for older
        // declaration metadata.
        type = parse_fn_type_str(X, s, base_len);
    } else if (base_len >= 2 && s[0] == '(' &&
               /* (p: T, ...) -> R — current-syntax function type literal.
                * The helper peeks past the matching `)` for ` -> ` so a
                * leading `(` without a trailing arrow falls through to the
                * tuple branch (`(T, U, ...)`) below. */
               has_arrow_after_paren(s, base_len)) {
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
            type = xr_type_new_error(X);
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
        // (BigInt, Logger, NetConn, NetListener, Range,
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
            if (!type) {
                const XaBuiltinObjectShape *object_shape =
                    xa_builtin_find_object_shape_by_name(name_buf);
                if (object_shape)
                    type = xa_builtin_object_shape_decl_type(X, object_shape);
            }
            if (!type) {
                const XaBuiltinEnum *enum_decl = xa_builtin_find_enum_by_name(name_buf);
                if (enum_decl)
                    type = xa_builtin_enum_decl_type(X, enum_decl, NULL);
            }
        }
        if (!type)
            type = xr_type_new_error(X);
    }

    if (type && nullable && !xr_type_intrinsically_includes_null(type)) {
        type = xr_type_make_nullable(X, type);
    }
    return type;
}

// Parse a "fn(p: T, ...): R" function type literal from a bounded slice.
// Operates on [s, s+len) so it can be used recursively inside larger type
// expressions where the inner fn is not NUL-terminated.
static XrType *parse_fn_type_str(XrVMRuntime *X, const char *s, size_t len) {
    XR_DCHECK(s != NULL, "parse_fn_type_str: NULL s");
    // Skip "fn" prefix and any spaces before '('.
    size_t i = 2;
    while (i < len && s[i] == ' ')
        i++;
    if (i >= len || s[i] != '(')
        return xr_type_new_error(X);
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
        return xr_type_new_error(X);

    // Parse parameter list between (open, close).
    XrType *param_types[16];
    XrParamMode param_modes[16];
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

            XrParamMode mode = XR_PARAM_READ;
            if (ts + 4 <= close && strncmp(s + ts, "ref ", 4) == 0) {
                mode = XR_PARAM_REF;
                ts += 4;
            } else if (ts + 5 <= close && strncmp(s + ts, "move ", 5) == 0) {
                mode = XR_PARAM_MOVE;
                ts += 5;
            }

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
            param_modes[param_count] = mode;
            if (!seen_optional)
                min_params = param_count + 1;
            param_count++;
            p = te;
        } else {
            while (p < close && s[p] != ',')
                p++;
            param_types[param_count] = xr_type_new_error(X);
            param_modes[param_count] = XR_PARAM_READ;
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
    if (fn_type) {
        fn_type->function.min_params = min_params;
        for (int k = 0; k < param_count; k++)
            xr_type_function_set_param_mode(fn_type, k, param_modes[k]);
    }
    if (params)
        xr_free(params);
    return fn_type;
}

// Parse full function signature: "(param: type, param2: type): ReturnType"
// Returns a complete function type with parameter types
XrType *xa_builtin_parse_full_signature(XrVMRuntime *X, const char *sig) {
    if (!sig)
        return xr_type_new_function(X, NULL, 0, xr_type_new_error(X), false);

    // Find parameter section: between '(' and matching ')'
    const char *open = strchr(sig, '(');
    if (!open)
        return xr_type_new_function(X, NULL, 0, xr_type_new_error(X), false);
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
    XrParamMode param_modes[16];
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

            XrParamMode mode = XR_PARAM_READ;
            if ((size_t) (close - type_start) >= 4 && strncmp(type_start, "ref ", 4) == 0) {
                mode = XR_PARAM_REF;
                type_start += 4;
            } else if ((size_t) (close - type_start) >= 5 && strncmp(type_start, "move ", 5) == 0) {
                mode = XR_PARAM_MOVE;
                type_start += 5;
            }

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
            param_modes[param_count] = mode;
            if (!seen_optional)
                min_params = param_count + 1;
            param_count++;
            p = type_end;
        } else {
            // No colon found, skip to next comma
            while (p < close && *p != ',')
                p++;
            param_optional[param_count] = false;
            param_types[param_count] = xr_type_new_error(X);
            param_modes[param_count] = XR_PARAM_READ;
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
    if (fn_type) {
        fn_type->function.min_params = min_params;
        for (int i = 0; i < param_count; i++)
            xr_type_function_set_param_mode(fn_type, i, param_modes[i]);
    }
    if (params)
        xr_free(params);
    return fn_type;
}

// Parse return type from signature string. The supported separators are:
//   "(param: type) -> ReturnType"      (current arrow syntax)
//   "(param: type): ReturnType"        (legacy colon syntax, still present in
//                                       some hand-authored builtin tables)
// Returns an XrType based on the return type portion after the separator.
XrType *xa_builtin_parse_return_type_from_sig(XrVMRuntime *X, const char *sig) {
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
