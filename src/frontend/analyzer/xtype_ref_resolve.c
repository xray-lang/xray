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
#include "xconsteval.h"
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

#define XA_CONSTEVAL_MAX_DEPTH 64

static bool ct_stack_contains(const uint32_t *stack, int depth, uint32_t id) {
    if (!stack || id == 0)
        return false;
    for (int i = 0; i < depth; i++) {
        if (stack[i] == id)
            return true;
    }
    return false;
}

static bool ct_fail(const char **err, const char *message) {
    if (err)
        *err = message;
    return false;
}

static bool ct_expect_kind(const XrCtValue *v, XrCtValueKind kind, const char **err,
                           const char *message) {
    if (v && v->kind == kind)
        return true;
    return ct_fail(err, message);
}

static bool ct_eval_impl(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                         const char **err, uint32_t *stack, int depth);

static bool ct_values_equal(const XrCtValue *a, const XrCtValue *b, bool *out) {
    if (!a || !b || !out || a->kind != b->kind)
        return false;
    switch (a->kind) {
        case XR_CT_INT:
            *out = a->as.int_val == b->as.int_val;
            return true;
        case XR_CT_FLOAT:
            *out = a->as.float_val == b->as.float_val;
            return true;
        case XR_CT_BOOL:
            *out = a->as.bool_val == b->as.bool_val;
            return true;
        case XR_CT_STRING:
            *out = strcmp(a->as.string_val ? a->as.string_val : "",
                          b->as.string_val ? b->as.string_val : "") == 0;
            return true;
        case XR_CT_CHAR:
            *out = a->as.char_val == b->as.char_val;
            return true;
        case XR_CT_NULL:
            *out = true;
            return true;
        default:
            return false;
    }
}

static XaSymbol *ct_lookup_const_symbol(XaAnalyzer *analyzer, const AstNode *expr) {
    if (!analyzer || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    XaSymbol *sym = NULL;
    if (expr->as.variable.symbol_id != 0)
        sym = xa_scope_lookup_by_id(analyzer->global_scope, expr->as.variable.symbol_id);
    if (!sym && analyzer->current_scope)
        sym = xa_scope_lookup(analyzer->current_scope, expr->as.variable.name);
    if (!sym && analyzer->global_scope)
        sym = xa_scope_lookup(analyzer->global_scope, expr->as.variable.name);
    return sym;
}

static bool ct_eval_unary(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                          const char **err, uint32_t *stack, int depth) {
    XrCtValue v = {0};
    if (!ct_eval_impl(analyzer, expr->as.unary.operand, &v, err, stack, depth + 1))
        return false;

    switch (expr->type) {
        case AST_UNARY_NEG:
            if (!ct_expect_kind(&v, XR_CT_INT, err, "unary '-' requires an integer constant"))
                return false;
            if (v.as.int_val == INT64_MIN)
                return ct_fail(err, "integer constant overflow");
            out->kind = XR_CT_INT;
            out->as.int_val = -v.as.int_val;
            return true;
        case AST_UNARY_BNOT:
            if (!ct_expect_kind(&v, XR_CT_INT, err, "unary '~' requires an integer constant"))
                return false;
            out->kind = XR_CT_INT;
            out->as.int_val = ~v.as.int_val;
            return true;
        case AST_UNARY_NOT:
            if (!ct_expect_kind(&v, XR_CT_BOOL, err, "unary '!' requires a bool constant"))
                return false;
            out->kind = XR_CT_BOOL;
            out->as.bool_val = !v.as.bool_val;
            return true;
        default:
            break;
    }
    return ct_fail(err, "unsupported unary consteval expression");
}

static bool ct_eval_binary(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                           const char **err, uint32_t *stack, int depth) {
    XrCtValue left = {0};
    XrCtValue right = {0};

    if (!ct_eval_impl(analyzer, expr->as.binary.left, &left, err, stack, depth + 1))
        return false;

    if (expr->type == AST_BINARY_AND || expr->type == AST_BINARY_OR) {
        if (!ct_expect_kind(&left, XR_CT_BOOL, err, "logical operator requires bool constants"))
            return false;
        if (expr->type == AST_BINARY_AND && !left.as.bool_val) {
            out->kind = XR_CT_BOOL;
            out->as.bool_val = false;
            return true;
        }
        if (expr->type == AST_BINARY_OR && left.as.bool_val) {
            out->kind = XR_CT_BOOL;
            out->as.bool_val = true;
            return true;
        }
    }

    if (!ct_eval_impl(analyzer, expr->as.binary.right, &right, err, stack, depth + 1))
        return false;

    switch (expr->type) {
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
            if (left.kind != XR_CT_INT || right.kind != XR_CT_INT)
                return ct_fail(err, "integer operator requires integer constants");
            int64_t l = left.as.int_val;
            int64_t r = right.as.int_val;
            out->kind = XR_CT_INT;
            switch (expr->type) {
                case AST_BINARY_ADD:
                    out->as.int_val = l + r;
                    return true;
                case AST_BINARY_SUB:
                    out->as.int_val = l - r;
                    return true;
                case AST_BINARY_MUL:
                    out->as.int_val = l * r;
                    return true;
                case AST_BINARY_DIV:
                    if (r == 0)
                        return ct_fail(err, "division by zero in constant expression");
                    out->as.int_val = l / r;
                    return true;
                case AST_BINARY_MOD:
                    if (r == 0)
                        return ct_fail(err, "modulo by zero in constant expression");
                    out->as.int_val = l % r;
                    return true;
                case AST_BINARY_BAND:
                    out->as.int_val = l & r;
                    return true;
                case AST_BINARY_BOR:
                    out->as.int_val = l | r;
                    return true;
                case AST_BINARY_BXOR:
                    out->as.int_val = l ^ r;
                    return true;
                case AST_BINARY_LSHIFT:
                    if (l < 0 || r < 0 || r >= 63)
                        return ct_fail(err, "bit shift is out of range");
                    out->as.int_val = l << r;
                    return true;
                case AST_BINARY_RSHIFT:
                    if (r < 0 || r >= 63)
                        return ct_fail(err, "bit shift is out of range");
                    out->as.int_val = l >> r;
                    return true;
                default:
                    break;
            }
            break;
        }
        case AST_BINARY_EQ:
        case AST_BINARY_NE: {
            bool eq = false;
            if (!ct_values_equal(&left, &right, &eq))
                return ct_fail(err, "equality requires constants of the same consteval kind");
            out->kind = XR_CT_BOOL;
            out->as.bool_val = expr->type == AST_BINARY_EQ ? eq : !eq;
            return true;
        }
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE: {
            if (left.kind != XR_CT_INT || right.kind != XR_CT_INT)
                return ct_fail(err, "comparison requires integer constants");
            int64_t l = left.as.int_val;
            int64_t r = right.as.int_val;
            out->kind = XR_CT_BOOL;
            switch (expr->type) {
                case AST_BINARY_LT:
                    out->as.bool_val = l < r;
                    return true;
                case AST_BINARY_LE:
                    out->as.bool_val = l <= r;
                    return true;
                case AST_BINARY_GT:
                    out->as.bool_val = l > r;
                    return true;
                case AST_BINARY_GE:
                    out->as.bool_val = l >= r;
                    return true;
                default:
                    break;
            }
            break;
        }
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            if (right.kind != XR_CT_BOOL)
                return ct_fail(err, "logical operator requires bool constants");
            out->kind = XR_CT_BOOL;
            out->as.bool_val = expr->type == AST_BINARY_AND
                                   ? (left.as.bool_val && right.as.bool_val)
                                   : (left.as.bool_val || right.as.bool_val);
            return true;
        default:
            break;
    }

    return ct_fail(err, "unsupported binary consteval expression");
}

static bool ct_eval_impl(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                         const char **err, uint32_t *stack, int depth) {
    if (!expr || !out)
        return ct_fail(err, "missing expression");
    if (depth > XA_CONSTEVAL_MAX_DEPTH)
        return ct_fail(err, "constant expression is too deeply nested");

    switch (expr->type) {
        case AST_COMPTIME_EXPR:
            return ct_eval_impl(analyzer, expr->as.comptime_expr.expr, out, err, stack, depth + 1);
        case AST_GROUPING:
            return ct_eval_impl(analyzer, expr->as.grouping, out, err, stack, depth + 1);
        case AST_LITERAL_INT:
            out->kind = XR_CT_INT;
            out->as.int_val = expr->as.literal.raw_value.int_val;
            return true;
        case AST_LITERAL_FLOAT:
            out->kind = XR_CT_FLOAT;
            out->as.float_val = expr->as.literal.raw_value.float_val;
            return true;
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            out->kind = XR_CT_BOOL;
            out->as.bool_val = expr->type == AST_LITERAL_TRUE;
            return true;
        case AST_LITERAL_STRING:
            out->kind = XR_CT_STRING;
            out->as.string_val = expr->as.literal.raw_value.string_val;
            return true;
        case AST_LITERAL_CHAR:
            out->kind = XR_CT_CHAR;
            out->as.char_val = expr->as.literal.raw_value.char_val;
            return true;
        case AST_LITERAL_NULL:
            out->kind = XR_CT_NULL;
            return true;
        case AST_UNARY_NEG:
        case AST_UNARY_BNOT:
        case AST_UNARY_NOT:
            return ct_eval_unary(analyzer, expr, out, err, stack, depth);
        case AST_VARIABLE: {
            XaSymbol *sym = ct_lookup_const_symbol(analyzer, expr);
            XaSymbolLinks *links = sym ? xa_analyzer_get_links(analyzer, sym) : NULL;
            if (!sym || !sym->is_const || !links || !links->const_initializer)
                return ct_fail(err, "identifier is not a compile-time const value");
            if (ct_stack_contains(stack, depth, sym->id))
                return ct_fail(err, "cyclic const expression");
            if (depth < XA_CONSTEVAL_MAX_DEPTH)
                stack[depth] = sym->id;
            return ct_eval_impl(analyzer, links->const_initializer, out, err, stack, depth + 1);
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
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            return ct_eval_binary(analyzer, expr, out, err, stack, depth);
        case AST_CALL_EXPR:
            return ct_fail(err, "function calls are not consteval-safe in this phase");
        case AST_BLOCK:
            return ct_fail(err, "comptime block engine is not implemented in this phase");
        default:
            break;
    }

    return ct_fail(err, "expression is not consteval-safe");
}

bool xa_consteval_expr(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out_value,
                       const char **out_error) {
    uint32_t stack[XA_CONSTEVAL_MAX_DEPTH + 1];
    memset(stack, 0, sizeof(stack));
    if (out_error)
        *out_error = NULL;
    XrCtValue tmp = {0};
    bool ok = ct_eval_impl(analyzer, expr, out_value ? out_value : &tmp, out_error, stack, 0);
    if (!ok && out_value)
        out_value->kind = XR_CT_NONE;
    return ok;
}

bool xa_consteval_int_expr(XaAnalyzer *analyzer, const AstNode *expr, int64_t *out_value,
                           const char **out_error) {
    XrCtValue value = {0};
    if (!xa_consteval_expr(analyzer, expr, &value, out_error))
        return false;
    if (value.kind != XR_CT_INT)
        return ct_fail(out_error, "expression is not an integer constant");
    if (out_value)
        *out_value = value.as.int_val;
    return true;
}

XR_FUNC bool xa_eval_const_int_expr(XaAnalyzer *analyzer, const AstNode *expr, int64_t *out_value,
                                    const char **out_error) {
    return xa_consteval_int_expr(analyzer, expr, out_value, out_error);
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
