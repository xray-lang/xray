/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_operator_overload.c - Static selection of arithmetic operator methods
 */

#include "xanalyzer_operator_overload.h"
#include "xanalyzer_visitor_internal.h"
#include "xanalyzer_symbol.h"
#include "../../base/xchecks.h"
#include "../../runtime/class/xclass_info.h"
#include "../../runtime/value/xtype.h"
#include "../parser/xast_nodes.h"

#include <stdio.h>

/* The member name an arithmetic operator resolves to. Operator methods are
 * stored under the operator spelling itself (xr_parse_operator_method), which
 * is also the name the runtime symbol table knows them by, so one string serves
 * lookup, lowering and dispatch. NULL for anything not overloadable here. */
static const char *operator_member_name(int op) {
    switch (op) {
        case AST_BINARY_ADD:
            return "+";
        case AST_BINARY_SUB:
            return "-";
        case AST_BINARY_MUL:
            return "*";
        case AST_BINARY_DIV:
            return "/";
        case AST_BINARY_MOD:
            return "%";
        default:
            return NULL;
    }
}

/* A receiver whose method table is known at this point: a concrete instance.
 * A bare class object, an interface, or an unsubstituted type parameter is not
 * one, and each of those keeps its existing permissive treatment so a generic
 * body still type-checks before monomorphization. */
static XrClassInfo *instance_class(XrType *type) {
    if (!type || !XR_TYPE_IS_INSTANCE(type) || type->is_nullable)
        return NULL;
    return type->instance.class_ref;
}

static void report(XaInferContext *ctx, struct AstNode *node, int code, const char *msg) {
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, msg, &loc);
}

XR_FUNC XaOperatorOverloadStatus xa_resolve_binary_operator_overload(
    XaInferContext *ctx, struct AstNode *node, struct XrType *left, struct XrType *right,
    struct XaSymbol **out_method, struct XrType **out_fn_type) {
    const char *op_name;
    XrClassInfo *receiver;
    XrClassInfo *owner = NULL;
    XaSymbol *method;
    XaSymbolLinks *links;
    XrType *fn_type;
    XrType *param_type;
    char msg[384];

    if (out_method)
        *out_method = NULL;
    if (out_fn_type)
        *out_fn_type = NULL;
    if (!ctx || !ctx->analyzer || !node || !left || !right)
        return XA_OPERATOR_OVERLOAD_NOT_APPLICABLE;

    op_name = operator_member_name(node->type);
    if (!op_name)
        return XA_OPERATOR_OVERLOAD_NOT_APPLICABLE;

    receiver = instance_class(left);
    if (!receiver) {
        /* An operator method belongs to the left operand's class, so an
         * instance on the right alone can never be dispatched. Saying so is
         * what keeps the backends honest: left permissive, the expression types
         * as unknown and the AOT reads the instance pointer as a number. */
        if (instance_class(right)) {
            snprintf(msg, sizeof(msg),
                     "operator '%s' is not defined for '%s' and '%s'; an operator method is "
                     "declared on the left operand's class, so put the '%s' value on the left or "
                     "call a named method",
                     op_name, xr_type_to_string(left), xr_type_to_string(right),
                     xr_type_to_string(right));
            report(ctx, node, XR_ERR_ANALYZE_TYPE_MISMATCH, msg);
            return XA_OPERATOR_OVERLOAD_REJECTED;
        }
        return XA_OPERATOR_OVERLOAD_NOT_APPLICABLE;
    }

    /* Operator methods are ordinary members named after the operator, so this
     * also finds one inherited from a base class. */
    method = xa_class_info_lookup_instance_member_owner(receiver, op_name, &owner);
    if (!method || method->kind != XA_SYM_METHOD) {
        snprintf(msg, sizeof(msg),
                 "operator '%s' is not defined for '%s' and '%s'; declare 'operator%s(other: %s)' "
                 "on '%s' or call a named method",
                 op_name, xr_type_to_string(left), xr_type_to_string(right), op_name,
                 xr_type_to_string(right), xr_type_to_string(left));
        report(ctx, node, XR_ERR_ANALYZE_TYPE_MISMATCH, msg);
        return XA_OPERATOR_OVERLOAD_REJECTED;
    }

    xa_check_member_visibility(ctx, node, method, owner);

    links = xa_analyzer_get_links(ctx->analyzer, method);
    fn_type = links ? links->type : NULL;
    if (!fn_type || !XR_TYPE_IS_FUNCTION(fn_type) || xr_type_get_param_count(fn_type) != 1) {
        /* `operator-` with no parameter is unary negation, which shares the
         * member name; a binary use of it has nothing to bind the right
         * operand to. */
        snprintf(msg, sizeof(msg),
                 "operator '%s' on '%s' takes no right-hand operand, so it cannot be used as a "
                 "binary operator",
                 op_name, xr_type_to_string(left));
        report(ctx, node, XR_ERR_ANALYZE_TYPE_MISMATCH, msg);
        return XA_OPERATOR_OVERLOAD_REJECTED;
    }

    param_type = xr_type_function_param_type(fn_type, 0);
    if (param_type && !XR_TYPE_IS_UNKNOWN(param_type) &&
        !xa_call_arg_type_assignable(param_type, right, xr_type_function_param_mode(fn_type, 0))) {
        /* `v * 2` against `operator*(scalar: float)`: the operand was inferred
         * before the method was known, so the literal took its own default type
         * instead of the parameter's. An argument position gives a numeric
         * literal its type from the parameter, and this is an argument
         * position. Re-inferring is confined to literals, which have no
         * side effects and no flow state to disturb. */
        AstNode *literal = XR_TYPE_IS_NUMERIC(param_type)
                               ? xa_contextual_numeric_literal_node(node->as.binary.right)
                               : NULL;
        if (literal) {
            XrType *saved_expected = ctx->expected_type;
            ctx->expected_type = param_type;
            right = xa_visit_infer_expr(ctx, node->as.binary.right);
            ctx->expected_type = saved_expected;
        }
        if (!literal ||
            !xa_call_arg_type_assignable(param_type, right,
                                         xr_type_function_param_mode(fn_type, 0))) {
            snprintf(msg, sizeof(msg),
                     "operator '%s' on '%s' expects '%s' on the right-hand side, not '%s'", op_name,
                     xr_type_to_string(left), xr_type_to_string(param_type),
                     xr_type_to_string(right));
            report(ctx, node, XR_ERR_ANALYZE_ARG_TYPE, msg);
            return XA_OPERATOR_OVERLOAD_REJECTED;
        }
    }

    if (out_method)
        *out_method = method;
    if (out_fn_type)
        *out_fn_type = fn_type;
    return XA_OPERATOR_OVERLOAD_RESOLVED;
}
