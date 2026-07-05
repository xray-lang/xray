/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_ref_resolve.c - Resolve XrTypeRef (syntax) to XrType* (runtime)
 *
 * Walks the XrTypeRef tree and maps each node to its runtime XrType*
 * counterpart.  Named types are resolved via class/prelude lookup;
 * primitives map 1-to-1 to their runtime singletons.
 */

#include "xtype_ref_resolve.h"
#include "xanalyzer.h"
#include "xanalyzer_builtin_interfaces.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../parser/xtype_ref.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/xisolate_api.h"
#include "../../base/xchecks.h"
#include "../../../stdlib/prelude/prelude.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Resolve child type refs recursively. */
static XrType *resolve_impl(XrVMRuntime *X, const XrTypeRef *t);

#define XA_CONST_EVAL_MAX_DEPTH 64

static bool xa_const_stack_contains(const uint32_t *stack, int depth, uint32_t id) {
    if (!stack || id == 0)
        return false;
    for (int i = 0; i < depth; i++) {
        if (stack[i] == id)
            return true;
    }
    return false;
}

static bool xa_eval_const_int_expr_impl(XaAnalyzer *analyzer, const AstNode *expr, int64_t *out,
                                        const char **err, uint32_t *stack, int depth) {
    if (!expr || !out) {
        if (err)
            *err = "missing expression";
        return false;
    }
    if (depth > XA_CONST_EVAL_MAX_DEPTH) {
        if (err)
            *err = "constant expression is too deeply nested";
        return false;
    }

    switch (expr->type) {
        case AST_LITERAL_INT:
            *out = expr->as.literal.raw_value.int_val;
            return true;

        case AST_GROUPING:
            return xa_eval_const_int_expr_impl(analyzer, expr->as.grouping, out, err, stack,
                                               depth + 1);

        case AST_UNARY_NEG: {
            int64_t v = 0;
            if (!xa_eval_const_int_expr_impl(analyzer, expr->as.unary.operand, &v, err, stack,
                                             depth + 1))
                return false;
            if (v == INT64_MIN) {
                if (err)
                    *err = "integer constant overflow";
                return false;
            }
            *out = -v;
            return true;
        }

        case AST_UNARY_BNOT: {
            int64_t v = 0;
            if (!xa_eval_const_int_expr_impl(analyzer, expr->as.unary.operand, &v, err, stack,
                                             depth + 1))
                return false;
            *out = ~v;
            return true;
        }

        case AST_VARIABLE: {
            if (!analyzer || !expr->as.variable.name) {
                if (err)
                    *err = "identifier is not a compile-time constant";
                return false;
            }
            XaSymbol *sym = NULL;
            if (expr->as.variable.symbol_id != 0)
                sym = xa_scope_lookup_by_id(analyzer->global_scope, expr->as.variable.symbol_id);
            if (!sym && analyzer->current_scope)
                sym = xa_scope_lookup(analyzer->current_scope, expr->as.variable.name);
            if (!sym && analyzer->global_scope)
                sym = xa_scope_lookup(analyzer->global_scope, expr->as.variable.name);
            XaSymbolLinks *links = sym ? xa_analyzer_get_links(analyzer, sym) : NULL;
            if (!sym || !sym->is_const || !links || !links->const_initializer) {
                if (err)
                    *err = "identifier is not a const integer";
                return false;
            }
            if (xa_const_stack_contains(stack, depth, sym->id)) {
                if (err)
                    *err = "cyclic const integer expression";
                return false;
            }
            if (depth < XA_CONST_EVAL_MAX_DEPTH)
                stack[depth] = sym->id;
            return xa_eval_const_int_expr_impl(analyzer, links->const_initializer, out, err, stack,
                                               depth + 1);
        }

        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT: {
            int64_t left = 0;
            int64_t right = 0;
            if (!xa_eval_const_int_expr_impl(analyzer, expr->as.binary.left, &left, err, stack,
                                             depth + 1) ||
                !xa_eval_const_int_expr_impl(analyzer, expr->as.binary.right, &right, err, stack,
                                             depth + 1)) {
                return false;
            }
            switch (expr->type) {
                case AST_BINARY_ADD:
                    *out = left + right;
                    return true;
                case AST_BINARY_SUB:
                    *out = left - right;
                    return true;
                case AST_BINARY_MUL:
                    *out = left * right;
                    return true;
                case AST_BINARY_DIV:
                    if (right == 0) {
                        if (err)
                            *err = "division by zero in constant expression";
                        return false;
                    }
                    *out = left / right;
                    return true;
                case AST_BINARY_MOD:
                    if (right == 0) {
                        if (err)
                            *err = "modulo by zero in constant expression";
                        return false;
                    }
                    *out = left % right;
                    return true;
                case AST_BINARY_BAND:
                    *out = left & right;
                    return true;
                case AST_BINARY_BOR:
                    *out = left | right;
                    return true;
                case AST_BINARY_BXOR:
                    *out = left ^ right;
                    return true;
                case AST_BINARY_LSHIFT:
                    if (left < 0 || right < 0 || right >= 63) {
                        if (err)
                            *err = "bit shift is out of range";
                        return false;
                    }
                    *out = left << right;
                    return true;
                case AST_BINARY_RSHIFT:
                    if (right < 0 || right >= 63) {
                        if (err)
                            *err = "bit shift is out of range";
                        return false;
                    }
                    *out = left >> right;
                    return true;
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }

    if (err)
        *err = "expression is not an integer constant";
    return false;
}

XR_FUNC bool xa_eval_const_int_expr(XaAnalyzer *analyzer, const AstNode *expr, int64_t *out_value,
                                    const char **out_error) {
    uint32_t stack[XA_CONST_EVAL_MAX_DEPTH + 1];
    memset(stack, 0, sizeof(stack));
    if (out_error)
        *out_error = NULL;
    return xa_eval_const_int_expr_impl(analyzer, expr, out_value, out_error, stack, 0);
}

static void xa_report_fixed_array_length_diag(XaAnalyzer *analyzer, const AstNode *expr,
                                              const char *message) {
    if (!analyzer || !message)
        return;
    XrLocation loc = {.file = analyzer->current_file,
                      .line = expr ? expr->line : 0,
                      .column = expr ? expr->column : 0};
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, message,
                               &loc);
}

/* Map a named type to its runtime XrType*.
 * Order: built-in interfaces → prelude → well-known singletons → class fallback. */
static XrType *resolve_named(XrVMRuntime *X, const char *name) {
    XR_DCHECK(name != NULL, "resolve_named: NULL name");

    /* Built-in interfaces (Comparable, Hashable, Stringable, Equatable, ...).
     * These must be resolved as XR_KIND_INTERFACE so generic-constraint checks
     * can match by interface name rather than treating them as plain classes.
     * Create a fresh XrType via the current isolate instead of reading a
     * global cache — multiple isolates would race on shared mutable state. */
    if (xa_is_builtin_interface_name(name))
        return xr_type_new_interface(X, name);

    if (strcmp(name, TYPE_NAME_BYTESPAN) == 0)
        return xr_type_new_bytespan(X);
    if (strcmp(name, TYPE_NAME_BYTEVIEW) == 0)
        return xr_type_new_byteview(X);
    if (strcmp(name, TYPE_NAME_SPAN) == 0)
        return xr_type_new_span(X, xr_type_new_unknown(NULL));
    if (strcmp(name, TYPE_NAME_VIEW) == 0)
        return xr_type_new_view(X, xr_type_new_unknown(NULL));

    /* Prelude lookup (Array, Map, Set, Channel, Json, Bytes, ...) */
    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(X);
    if (symbols) {
        const XrPreludeTypeEntry *entry = xr_prelude_lookup_type(symbols, name, strlen(name));
        if (entry) {
            switch ((XrPreludeKind) entry->kind) {
                case XR_PRELUDE_KIND_SIMPLE:
                    return xr_type_new_named_instance(X, entry->name);
                case XR_PRELUDE_KIND_SINGLETON:
                    if (strcmp(entry->name, "Json") == 0)
                        return xr_type_new_json(X);
                    return xr_type_new_named_instance(X, entry->name);
                case XR_PRELUDE_KIND_GENERIC_1:
                case XR_PRELUDE_KIND_GENERIC_2:
                    /* Bare name without type args — use unknown placeholders */
                    if (strcmp(entry->name, "Bytes") == 0)
                        return xr_type_new_bytes(X);
                    if (strcmp(entry->name, "Array") == 0)
                        return xr_type_new_array(X, xr_type_new_unknown(NULL));
                    if (strcmp(entry->name, "Set") == 0)
                        return xr_type_new_set(X, xr_type_new_unknown(NULL));
                    if (strcmp(entry->name, "Channel") == 0)
                        return xr_type_new_channel(X, xr_type_new_unknown(NULL));
                    if (strcmp(entry->name, "Map") == 0)
                        return xr_type_new_map(X, xr_type_new_unknown(NULL),
                                               xr_type_new_unknown(NULL));
                    return xr_type_new_named_instance(X, entry->name);
            }
        }
    }

    /* Well-known names not in prelude */
    if (strcmp(name, "Task") == 0)
        return xr_type_new_task(X, xr_type_new_unknown(NULL));
    if (strcmp(name, "PanicInfo") == 0)
        return xr_type_new_named_instance(X, "PanicInfo");
    if (strcmp(name, TYPE_NAME_BUFFER) == 0)
        return xr_type_new_named_instance(X, TYPE_NAME_BUFFER);
    if (strcmp(name, "unknown") == 0)
        return xr_type_new_unknown(X);

    /* Default: treat as class name */
    return xr_type_new_class(X, name);
}

/* Map a generic type (Name<T1, ...>) to its runtime XrType*. */
static XrType *resolve_generic(XrVMRuntime *X, const XrTypeRef *t) {
    XR_DCHECK(t != NULL && t->name != NULL, "resolve_generic: NULL");
    const char *name = t->name;
    int nargs = t->nchildren;

    /* Resolve all type arguments first */
    XrType *stack_args[16];
    XrType **args =
        (nargs <= 16) ? stack_args : (XrType **) xr_malloc((size_t) nargs * sizeof(XrType *));
    if (nargs > 0 && !args)
        return xr_type_new_unknown(NULL);
    for (int i = 0; i < nargs; i++)
        args[i] = resolve_impl(X, t->children[i]);

    /* Dispatch to known container constructors */
    XrType *result = NULL;
    if (strcmp(name, "Array") == 0 && nargs >= 1) {
        result = xr_type_new_array(X, args[0]);
    } else if (strcmp(name, TYPE_NAME_SPAN) == 0 && nargs >= 1) {
        result = xr_type_new_span(X, args[0]);
    } else if (strcmp(name, TYPE_NAME_VIEW) == 0 && nargs >= 1) {
        result = xr_type_new_view(X, args[0]);
    } else if (strcmp(name, "Set") == 0 && nargs >= 1) {
        result = xr_type_new_set(X, args[0]);
    } else if (strcmp(name, "Channel") == 0 && nargs >= 1) {
        result = xr_type_new_channel(X, args[0]);
    } else if (strcmp(name, "Map") == 0 && nargs >= 2) {
        result = xr_type_new_map(X, args[0], args[1]);
    } else if (strcmp(name, "Task") == 0 && nargs >= 1) {
        result = xr_type_new_task(X, args[0]);
    } else if (strcmp(name, "RawPtr") == 0 && nargs >= 1) {
        result = xr_type_new_pointer(X, args[0], false);  // const raw pointer
    } else if (strcmp(name, "RawMut") == 0 && nargs >= 1) {
        result = xr_type_new_pointer(X, args[0], true);  // mutable raw pointer
    } else if (strcmp(name, "CFn") == 0 && nargs == 1 && args[0] &&
               args[0]->kind == XR_KIND_FUNCTION) {
        result = xr_type_copy(X, args[0]);
        if (result)
            result->function.is_c_abi = true;
    } else if (xa_is_builtin_interface_name(name)) {
        /* Built-in interface with type args: e.g. Iterable<int>. Create a fresh
         * generic interface type via the current isolate. */
        result = xr_type_new_generic_interface(X, name, args, nargs);
    } else {
        /* Generic class instance — fallback for user types */
        result = xr_type_new_generic_instance(X, name, NULL, args, nargs);
    }

    if (args != stack_args)
        xr_free(args);
    return result;
}

static XrType *resolve_impl(XrVMRuntime *X, const XrTypeRef *t) {
    if (!t)
        return xr_type_new_unknown(NULL);

    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_INT:
            return xr_type_new_int(NULL);
        case XR_TREF_FLOAT:
            return xr_type_new_float(NULL);
        case XR_TREF_STRING:
            return xr_type_new_string(NULL);
        case XR_TREF_BOOL:
            return xr_type_new_bool(NULL);
        case XR_TREF_CHAR:
            return xr_type_new_char(NULL);
        case XR_TREF_UNIT:
            return xr_type_new_unit(NULL);
        case XR_TREF_NULL:
            return xr_type_new_null(NULL);
        case XR_TREF_UNKNOWN:
            return xr_type_new_unknown(NULL);

        case XR_TREF_INT_WIDTH:
            return xr_type_new_int_width(X, t->native_width);
        case XR_TREF_FLOAT_WIDTH:
            return xr_type_new_float_width(X, t->native_width);

        case XR_TREF_NAMED:
            return resolve_named(X, t->name);
        case XR_TREF_GENERIC:
            return resolve_generic(X, t);

        case XR_TREF_OPTIONAL: {
            XrType *inner = resolve_impl(X, t->children[0]);
            return xr_type_new_optional(X, inner);
        }

        case XR_TREF_UNION: {
            XrType *members[XR_UNION_MAX_MEMBERS];
            int count = t->nchildren < XR_UNION_MAX_MEMBERS ? t->nchildren : XR_UNION_MAX_MEMBERS;
            for (int i = 0; i < count; i++)
                members[i] = resolve_impl(X, t->children[i]);
            return xr_type_new_union(X, members, count);
        }

        case XR_TREF_FUNCTION: {
            int nparam = t->nchildren > 0 ? t->nchildren - 1 : 0;
            XrType *stack_params[16];
            XrType **params = (nparam <= 16)
                                  ? stack_params
                                  : (XrType **) xr_malloc((size_t) nparam * sizeof(XrType *));
            if (nparam > 0 && !params)
                return xr_type_new_unknown(NULL);
            for (int i = 0; i < nparam; i++)
                params[i] = resolve_impl(X, t->children[i]);
            XrType *ret = t->nchildren > 0 ? resolve_impl(X, t->children[t->nchildren - 1])
                                           : xr_type_new_unit(NULL);
            XrType *result =
                xr_type_new_function(X, nparam > 0 ? params : NULL, nparam, ret, false);
            if (params != stack_params)
                xr_free(params);
            return result;
        }

        case XR_TREF_TUPLE: {
            int count = t->nchildren;
            XrType *stack_elems[16];
            XrType **elems = (count <= 16)
                                 ? stack_elems
                                 : (XrType **) xr_malloc((size_t) count * sizeof(XrType *));
            if (count > 0 && !elems)
                return xr_type_new_unknown(NULL);
            for (int i = 0; i < count; i++)
                elems[i] = resolve_impl(X, t->children[i]);
            XrType *result = xr_type_new_tuple(X, elems, count);
            if (elems != stack_elems)
                xr_free(elems);
            return result;
        }

        case XR_TREF_OBJECT: {
            const char **names = (const char **) t->field_names;
            int count = t->nchildren;
            XrType *stack_types[16];
            XrType **types = (count <= 16)
                                 ? stack_types
                                 : (XrType **) xr_malloc((size_t) count * sizeof(XrType *));
            if (count > 0 && !types)
                return xr_type_new_unknown(NULL);
            for (int i = 0; i < count; i++)
                types[i] = resolve_impl(X, t->children[i]);
            bool is_sealed = !t->extensible;
            XrType *result = xr_type_new_record_with_fields(X, names, types, count, is_sealed);
            if (result && t->field_readonly)
                xr_type_set_object_field_readonly(X, result, t->field_readonly, count);
            if (result && t->name)
                xr_type_set_object_type_name(X, result, t->name);
            if (types != stack_types)
                xr_free(types);
            return result;
        }

        case XR_TREF_FIXED_ARRAY: {
            XrType *elem =
                t->nchildren > 0 ? resolve_impl(X, t->children[0]) : xr_type_new_unknown(NULL);
            if (t->fixed_length <= 0 || t->fixed_length > (int) UINT16_MAX)
                return xr_type_new_unknown(X);
            return xr_type_new_fixed_array(X, elem, (int) t->fixed_length);
        }

        case XR_TREF_TYPE_PARAM:
            return xr_type_new_type_param(X, t->name, 0);
    }

    return xr_type_new_unknown(NULL);
}

XR_FUNC XrType *xr_tref_resolve(XrVMRuntime *X, const XrTypeRef *tref) {
    return resolve_impl(X, tref);
}

/* Look up `name` as a declaration-backed type in analyzer scopes; on hit,
 * return the canonical XrType (carrying the inheritance chain for classes,
 * enum identity for enum values, or the interface singleton for interfaces).
 * Returns NULL when no matching symbol is registered. */
static XaSymbol *resolve_type_symbol(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;

    XaScope *start = analyzer->current_scope ? analyzer->current_scope : analyzer->global_scope;
    XaSymbol *sym = xa_scope_lookup(start, name);
    if (!sym)
        return NULL;
    if (sym->kind == XA_SYM_CLASS || sym->kind == XA_SYM_ENUM)
        return sym;
    if (sym->kind == XA_SYM_IMPORT) {
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        XrType *type = links ? links->type : NULL;
        if (links && links->module_name && strcmp(links->module_name, "sync") == 0) {
            const char *class_name = links->import_member_name ? links->import_member_name : name;
            if (strcmp(class_name, "Semaphore") == 0 || strcmp(class_name, "CountdownLatch") == 0 ||
                strcmp(class_name, "EventCount") == 0 || strcmp(class_name, "WorkQueue") == 0 ||
                strcmp(class_name, "ResultGroup") == 0) {
                return sym;
            }
        }
        if (type && (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE ||
                     type->kind == XR_KIND_INTERFACE || type->kind == XR_KIND_ENUM)) {
            return sym;
        }
    }
    return NULL;
}

static XrType *resolve_type_ref_symbol_type(XaAnalyzer *analyzer, const char *name) {
    XaSymbol *sym = resolve_type_symbol(analyzer, name);
    if (!sym)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (!links)
        return NULL;

    if (sym->kind == XA_SYM_IMPORT && links->module_name &&
        strcmp(links->module_name, "sync") == 0) {
        const char *class_name = links->import_member_name ? links->import_member_name : name;
        if (strcmp(class_name, "Semaphore") == 0 || strcmp(class_name, "CountdownLatch") == 0 ||
            strcmp(class_name, "EventCount") == 0 || strcmp(class_name, "WorkQueue") == 0 ||
            strcmp(class_name, "ResultGroup") == 0) {
            return xr_type_new_named_instance(analyzer->isolate, class_name);
        }
    }

    if (!links->type)
        return NULL;

    if (sym->kind == XA_SYM_ENUM)
        return links->type;
    if (links->type->kind == XR_KIND_INTERFACE)
        return links->type;
    if (links->type->kind == XR_KIND_INSTANCE)
        return links->type;
    if (links->type->kind == XR_KIND_CLASS) {
        if (links->class_info)
            return xr_type_new_instance(analyzer->isolate, links->class_info);
        if (links->type->instance.class_name)
            return xr_type_new_named_instance(analyzer->isolate, links->type->instance.class_name);
        return xr_type_new_instance(analyzer->isolate, NULL);
    }
    return NULL;
}

XR_FUNC XrType *xr_tref_resolve_in_analyzer(XaAnalyzer *analyzer, const XrTypeRef *tref) {
    if (!tref)
        return xr_type_new_unknown(NULL);
    if (!analyzer)
        return resolve_impl(NULL, tref);

    /* Named class lookup preserves the inheritance chain that
     * xr_type_new_class() would otherwise drop. */
    if (tref->kind == XR_TREF_NAMED && tref->name) {
        XrType *cls = resolve_type_ref_symbol_type(analyzer, tref->name);
        if (cls)
            return cls;
    }

    /* Generic form: preserve declaration-backed class/interface identity so
     * member lookup and conformance checks do not fall back to a bare name. */
    if (tref->kind == XR_TREF_GENERIC && tref->name) {
        XaSymbol *sym = resolve_type_symbol(analyzer, tref->name);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(analyzer, sym) : NULL;
        const char *sync_class_name = NULL;
        if (sym && sym->kind == XA_SYM_IMPORT && links && links->module_name &&
            strcmp(links->module_name, "sync") == 0) {
            const char *class_name =
                links->import_member_name ? links->import_member_name : tref->name;
            if (strcmp(class_name, "Semaphore") == 0 || strcmp(class_name, "CountdownLatch") == 0 ||
                strcmp(class_name, "EventCount") == 0 || strcmp(class_name, "WorkQueue") == 0 ||
                strcmp(class_name, "ResultGroup") == 0) {
                sync_class_name = class_name;
            }
        }
        XrType *head = links ? links->type : NULL;
        if (sync_class_name) {
            int nargs = tref->nchildren;
            XrType *stack_args[8];
            XrType **args =
                (nargs <= 8) ? stack_args : (XrType **) xr_malloc(sizeof(XrType *) * nargs);
            if (args) {
                for (int i = 0; i < nargs; i++)
                    args[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);
                XrType *result = xr_type_new_generic_instance(analyzer->isolate, sync_class_name,
                                                              NULL, args, nargs);
                if (args != stack_args)
                    xr_free(args);
                return result;
            }
        }
        if (head && (head->kind == XR_KIND_INTERFACE || head->kind == XR_KIND_CLASS ||
                     head->kind == XR_KIND_INSTANCE)) {
            int nargs = tref->nchildren;
            XrType *stack_args[8];
            XrType **args =
                (nargs <= 8) ? stack_args : (XrType **) xr_malloc(sizeof(XrType *) * nargs);
            if (args) {
                for (int i = 0; i < nargs; i++)
                    args[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);
                XrType *result =
                    head->kind == XR_KIND_INTERFACE
                        ? xr_type_new_generic_interface(analyzer->isolate, tref->name, args, nargs)
                        : xr_type_new_generic_instance(analyzer->isolate, tref->name,
                                                       links->class_info, args, nargs);
                if (args != stack_args)
                    xr_free(args);
                return result;
            }
        }
    }

    if (tref->kind == XR_TREF_FIXED_ARRAY) {
        XrType *elem = tref->nchildren > 0
                           ? xr_tref_resolve_in_analyzer(analyzer, tref->children[0])
                           : xr_type_new_unknown(NULL);
        int64_t length = tref->fixed_length;
        const AstNode *length_expr = tref->fixed_length_expr;
        if (length_expr) {
            const char *err = NULL;
            if (!xa_eval_const_int_expr(analyzer, length_expr, &length, &err)) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "fixed array length must be a compile-time integer expression%s%s",
                         err ? ": " : "", err ? err : "");
                xa_report_fixed_array_length_diag(analyzer, length_expr, msg);
                return xr_type_new_unknown(NULL);
            }
        }
        if (length <= 0) {
            xa_report_fixed_array_length_diag(analyzer, length_expr,
                                              "fixed array length must be greater than zero");
            return xr_type_new_unknown(NULL);
        }
        if (length > UINT16_MAX) {
            xa_report_fixed_array_length_diag(
                analyzer, length_expr, "fixed array length exceeds maximum of 65535 elements");
            return xr_type_new_unknown(NULL);
        }
        return xr_type_new_fixed_array(analyzer->isolate, elem, (int) length);
    }

    return resolve_impl(analyzer->isolate, tref);
}
