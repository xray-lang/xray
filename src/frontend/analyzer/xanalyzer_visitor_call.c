/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_call.c - Pass 2 visitor for function/method calls
 *
 * KEY CONCEPT:
 *   xa_visit_call() is the largest single visitor in the analyzer
 *   (~330 lines): it handles argument count and type checking, generic
 *   type-argument validation, type-parameter inference for generic
 *   functions, and callback-type inference for container methods
 *   (map / filter / reduce / forEach / find / every / some).
 *
 *   This file holds the call-shaped subset of the analyzer visitor
 *   (call expression inference, generic substitution, callback-type
 *   inference for container methods).
 */

#include "xanalyzer_visitor_internal.h"
#include "xanalyzer_ast_visitor.h"
#include "xtype_ref_resolve.h"
#include "xanalyzer_mono.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xchecks.h"
#include "../../base/xconstants.h"
#include "../../base/xhashmap.h"

static XrType *xa_call_raw_pointer_type_namespace(XaInferContext *ctx, AstNode *object) {
    if (!ctx || !object || object->type != AST_NEW_EXPR)
        return NULL;
    NewExprNode *ne = &object->as.new_expr;
    if (ne->module_name || !ne->class_name || ne->type_arg_count != 1 || !ne->type_args ||
        !ne->type_args[0])
        return NULL;
    bool is_mut = false;
    if (strcmp(ne->class_name, "RawPtr") == 0) {
        is_mut = false;
    } else if (strcmp(ne->class_name, "RawMut") == 0) {
        is_mut = true;
    } else {
        return NULL;
    }
    XrType *pointee = xr_tref_resolve(ctx->analyzer->isolate, ne->type_args[0]);
    if (!pointee)
        pointee = xr_type_new_unknown(ctx->analyzer->isolate);
    return xr_type_new_pointer(ctx->analyzer->isolate, pointee, is_mut);
}

static bool xa_object_literal_bool_field(AstNode *node, const char *field_name, bool *out_value) {
    if (!node || node->type != AST_OBJECT_LITERAL || !field_name || !out_value)
        return false;
    ObjectLiteralNode *obj = &node->as.object_literal;
    for (int i = 0; i < obj->count; i++) {
        if (obj->computed && obj->computed[i])
            continue;
        AstNode *key = obj->keys ? obj->keys[i] : NULL;
        AstNode *value = obj->values ? obj->values[i] : NULL;
        if (!key || key->type != AST_LITERAL_STRING || !value)
            continue;
        if (strcmp(key->as.literal.raw_value.string_val, field_name) != 0)
            continue;
        if (value->type == AST_LITERAL_TRUE) {
            *out_value = true;
            return true;
        }
        if (value->type == AST_LITERAL_FALSE) {
            *out_value = false;
            return true;
        }
    }
    return false;
}

static bool xa_type_is_c_callback(const XrType *type) {
    return type && XR_TYPE_IS_C_FUNCTION(type);
}

static void xa_report_c_callback_requires_top_level(XaInferContext *ctx, AstNode *site, int slot) {
    XrLocation loc = {
        .file = ctx->file_path, .line = site ? site->line : 0, .column = site ? site->column : 0};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Argument %d: CFn callback must be a top-level non-extern function name", slot + 1);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static bool xa_c_callback_arg_is_top_level_function(XaInferContext *ctx, AstNode *arg_node) {
    if (!ctx || !ctx->analyzer || !arg_node || arg_node->type != AST_VARIABLE)
        return false;
    const char *name = arg_node->as.variable.name;
    XaSymbol *sym = name ? xa_lookup_visible_symbol(ctx, name) : NULL;
    if (!sym || sym->kind != XA_SYM_FUNCTION || !sym->scope ||
        sym->scope->kind != XA_SCOPE_GLOBAL || sym->is_builtin)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    return !links || !links->is_extern;
}

static bool xa_c_callback_signature_matches(XrType *callback_type, XrType *arg_type) {
    if (!xa_type_is_c_callback(callback_type) || !arg_type || !XR_TYPE_IS_FUNCTION(arg_type))
        return false;
    if (arg_type->function.is_c_abi || arg_type->function.is_variadic)
        return false;
    if (callback_type->function.param_count != arg_type->function.param_count)
        return false;
    if (!xr_type_equals(callback_type->function.return_type, arg_type->function.return_type))
        return false;
    for (int i = 0; i < callback_type->function.param_count; i++) {
        XrType *want =
            callback_type->function.param_types ? callback_type->function.param_types[i] : NULL;
        XrType *got = arg_type->function.param_types ? arg_type->function.param_types[i] : NULL;
        if (!xr_type_equals(want, got))
            return false;
    }
    return true;
}

static void xa_report_c_callback_signature_mismatch(XaInferContext *ctx, AstNode *site, int slot,
                                                    XrType *callback_type, XrType *arg_type) {
    XrLocation loc = {
        .file = ctx->file_path, .line = site ? site->line : 0, .column = site ? site->column : 0};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Argument %d: CFn callback type '%s' requires top-level function with exact "
             "signature '%s'",
             slot + 1, arg_type ? xr_type_to_string(arg_type) : "<unknown>",
             callback_type ? xr_type_to_string(callback_type) : "<unknown>");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static bool xa_is_module_call(CallExprNode *call, const char *module_name, const char *func_name) {
    if (!call || !module_name || !func_name || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || strcmp(ma->name, func_name) != 0 || !ma->object ||
        ma->object->type != AST_VARIABLE)
        return false;
    return strcmp(ma->object->as.variable.name, module_name) == 0;
}

static bool xa_call_is_sys_thread_spawn(const CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *spawn = &call->callee->as.member_access;
    if (!spawn->name || strcmp(spawn->name, "spawn") != 0 || !spawn->object ||
        spawn->object->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *thread = &spawn->object->as.member_access;
    if (!thread->name || strcmp(thread->name, "Thread") != 0 || !thread->object ||
        thread->object->type != AST_VARIABLE)
        return false;
    const char *module_name = thread->object->as.variable.name;
    return module_name && strcmp(module_name, "sys") == 0;
}

static int xa_thread_spawn_body_arg_index(const CallExprNode *call) {
    if (!call)
        return -1;
    if (call->arg_count == 1)
        return 0;
    if (call->arg_count == 2)
        return 1;
    return -1;
}

static bool xa_thread_spawn_options_is_int_literal(AstNode *node, int64_t *out) {
    if (!node || node->type != AST_LITERAL_INT)
        return false;
    if (out)
        *out = node->as.literal.raw_value.int_val;
    return true;
}

static bool xa_thread_spawn_options_is_string_literal(AstNode *node) {
    return node && node->type == AST_LITERAL_STRING;
}

static void xa_check_thread_spawn_affinity_option(XaInferContext *ctx, AstNode *node,
                                                  AstNode *value) {
    if (!ctx || !node)
        return;
    XrLocation floc = {.file = ctx->file_path,
                       .line = value ? value->line : node->line,
                       .column = value ? value->column : node->column};
    if (!value || value->type != AST_ARRAY_LITERAL) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "sys.Thread.spawn ThreadOptions.affinity must be an array literal of non-negative "
            "integer literals",
            &floc);
        return;
    }

    ArrayLiteralNode *arr = &value->as.array_literal;
    if (arr->count > XR_THREAD_AFFINITY_MAX) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "sys.Thread.spawn ThreadOptions.affinity supports at most 32 CPU hints", &floc);
        return;
    }
    for (int i = 0; i < arr->count; i++) {
        AstNode *elem = arr->elements ? arr->elements[i] : NULL;
        int64_t cpu = 0;
        if (!xa_thread_spawn_options_is_int_literal(elem, &cpu) || cpu < 0) {
            XrLocation eloc = {.file = ctx->file_path,
                               .line = elem ? elem->line : value->line,
                               .column = elem ? elem->column : value->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                "sys.Thread.spawn ThreadOptions.affinity must contain only non-negative integer "
                "literals",
                &eloc);
        }
    }
}

static void xa_check_thread_spawn_options(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    if (node->type != AST_STRUCT_LITERAL) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "sys.Thread.spawn options must be ThreadOptions{ stackSize: <int>, name: <string>, "
            "affinity: [<int>, ...] }",
            &loc);
        return;
    }

    StructLiteralNode *sl = &node->as.struct_literal;
    if (!sl->struct_name || strcmp(sl->struct_name, "ThreadOptions") != 0) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "sys.Thread.spawn options must use ThreadOptions{ ... }", &loc);
        return;
    }

    for (int i = 0; i < sl->field_count; i++) {
        AstNode *value = sl->field_values ? sl->field_values[i] : NULL;
        const char *name = sl->field_names ? sl->field_names[i] : NULL;
        if (value)
            xa_visit_infer_expr(ctx, value);
        if (!name || (strcmp(name, "stackSize") != 0 && strcmp(name, "name") != 0 &&
                      strcmp(name, "affinity") != 0)) {
            XrLocation floc = {.file = ctx->file_path,
                               .line = value ? value->line : node->line,
                               .column = value ? value->column : node->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                "sys.Thread.spawn ThreadOptions currently supports only stackSize/name/affinity",
                &floc);
            continue;
        }
        if (strcmp(name, "stackSize") == 0) {
            int64_t stack_size = 0;
            if (!xa_thread_spawn_options_is_int_literal(value, &stack_size) || stack_size < 0) {
                XrLocation floc = {.file = ctx->file_path,
                                   .line = value ? value->line : node->line,
                                   .column = value ? value->column : node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE,
                                           "sys.Thread.spawn ThreadOptions.stackSize must be a "
                                           "non-negative integer literal",
                                           &floc);
            }
        } else if (strcmp(name, "name") == 0) {
            if (!xa_thread_spawn_options_is_string_literal(value)) {
                XrLocation floc = {.file = ctx->file_path,
                                   .line = value ? value->line : node->line,
                                   .column = value ? value->column : node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                    "sys.Thread.spawn ThreadOptions.name must be a string literal", &floc);
            }
        } else if (strcmp(name, "affinity") == 0) {
            xa_check_thread_spawn_affinity_option(ctx, node, value);
        }
    }
}

static void xa_check_spawn_call_boundary_args(XaInferContext *ctx, AstNode *boundary_node,
                                              CallExprNode *call) {
    if (!ctx || !call)
        return;
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *arg = call->arguments[i];
        if (!arg)
            continue;
        if (arg->type == AST_SPREAD_EXPR) {
            XrType *src = xa_analyzer_get_node_type(ctx->analyzer, arg->as.spread_expr.expr);
            if (!src)
                src = xa_visit_infer_expr(ctx, arg->as.spread_expr.expr);
            if (src && XR_TYPE_IS_TUPLE(src)) {
                for (int j = 0; j < src->tuple.element_count; j++) {
                    xa_check_boundary_transfer_arg(ctx, boundary_node, arg,
                                                   src->tuple.element_types[j],
                                                   "sys.Thread.spawn argument");
                }
            }
            continue;
        }
        XrType *arg_type = xa_analyzer_get_node_type(ctx->analyzer, arg);
        if (!arg_type)
            arg_type = xa_visit_infer_expr(ctx, arg);
        xa_check_boundary_transfer_arg(ctx, boundary_node, arg, arg_type,
                                       "sys.Thread.spawn argument");
    }
}

typedef struct XaThreadSpawnSyncScan {
    XaInferContext *ctx;
    bool reported;
} XaThreadSpawnSyncScan;

static void xa_thread_spawn_sync_scan_pre(AstNode *node, void *ud) {
    XaThreadSpawnSyncScan *scan = (XaThreadSpawnSyncScan *) ud;
    if (!scan || scan->reported || !scan->ctx || !node)
        return;
    const char *feature = NULL;
    if (node->type == AST_AWAIT_EXPR)
        feature = "await";
    else if (node->type == AST_YIELD_STMT)
        feature = "yield";
    if (!feature)
        return;

    XrLocation loc = {.file = scan->ctx->file_path, .line = node->line, .column = node->column};
    char msg[160];
    snprintf(msg, sizeof(msg),
             "sys.Thread.spawn body cannot suspend; %s is not allowed in an OS thread body",
             feature);
    xa_analyzer_add_diagnostic(scan->ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE,
                               msg, &loc);
    scan->reported = true;
}

static AstNode *xa_thread_spawn_inline_body(AstNode *body) {
    if (!body)
        return NULL;
    if (body->type == AST_FUNCTION_DECL)
        return body->as.function_decl.body;
    if (body->type == AST_FUNCTION_EXPR)
        return body->as.function_expr.body;
    if (body->type == AST_CALL_EXPR) {
        AstNode *callee = body->as.call_expr.callee;
        if (callee && callee->type == AST_FUNCTION_DECL)
            return callee->as.function_decl.body;
        if (callee && callee->type == AST_FUNCTION_EXPR)
            return callee->as.function_expr.body;
    }
    return NULL;
}

static void xa_check_thread_spawn_sync_body(XaInferContext *ctx, AstNode *body) {
    AstNode *inline_body = xa_thread_spawn_inline_body(body);
    if (!inline_body)
        return;
    XaThreadSpawnSyncScan scan = {.ctx = ctx, .reported = false};
    xa_ast_walk(inline_body, xa_thread_spawn_sync_scan_pre, NULL, &scan);
}

static XrType *xa_visit_sys_thread_spawn_call(XaInferContext *ctx, AstNode *node,
                                              CallExprNode *call) {
    xa_freestanding_report_unavailable(ctx, node, "sys.Thread.spawn", "OS threads are hosted-only");

    int body_index = xa_thread_spawn_body_arg_index(call);
    if (body_index < 0 || !call->arguments[body_index]) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "sys.Thread.spawn expects fn or (ThreadOptions, fn)", &loc);
        return xr_type_new_unknown(NULL);
    }

    if (body_index == 1 && call->arguments[0])
        xa_check_thread_spawn_options(ctx, call->arguments[0]);

    AstNode *body = call->arguments[body_index];
    XrType *body_type = xa_visit_infer_expr(ctx, body);
    if (body && body->type == AST_CALL_EXPR)
        xa_check_spawn_call_boundary_args(ctx, node, &body->as.call_expr);
    check_coro_capture(ctx, body, node->line);
    xa_check_thread_spawn_sync_body(ctx, body);

    XrType *result_type = xr_type_new_unit(NULL);
    if (body_type && XR_TYPE_IS_FUNCTION(body_type) && body_type->function.return_type)
        result_type = body_type->function.return_type;
    else if (body_type && !XR_TYPE_IS_FUNCTION(body_type))
        result_type = body_type;

    XrType **args = (XrType **) xr_malloc(sizeof(XrType *));
    if (!args)
        return xr_type_new_named_instance(ctx->analyzer->isolate, "Thread");
    args[0] = result_type ? result_type : xr_type_new_unknown(NULL);
    return xr_type_new_generic_instance(ctx->analyzer->isolate, "Thread", NULL, args, 1);
}

static bool xa_type_is_bytespan_view(XrType *type) {
    if (!type || !XR_TYPE_IS_SPAN(type) || !type->container.element_type)
        return false;
    XrType *elem = type->container.element_type;
    return XR_TYPE_IS_INT(elem) && elem->native_width == XR_NATIVE_U8;
}

static bool xa_type_is_raw_u8_ptr_view(XrType *type) {
    if (!type || !XR_TYPE_IS_POINTER(type) || !type->container.element_type)
        return false;
    XrType *elem = type->container.element_type;
    return XR_TYPE_IS_INT(elem) && elem->native_width == XR_NATIVE_U8;
}

static bool xa_call_is_bytespan_load_le(CallExprNode *call, XrType *receiver_type) {
    if (!call || !receiver_type || !xa_type_is_bytespan_view(receiver_type) || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name &&
           (strcmp(ma->name, "loadLE") == 0 || strcmp(ma->name, "loadLEUnchecked") == 0);
}

static bool xa_call_is_rawptr_load_le_unchecked(CallExprNode *call, XrType *receiver_type) {
    if (!call || !receiver_type || !xa_type_is_raw_u8_ptr_view(receiver_type) || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "loadLEUnchecked") == 0;
}

static bool xa_type_is_supported_load_le_result(XrType *type) {
    return type && XR_TYPE_IS_INT(type) &&
           (type->native_width == XR_NATIVE_U16 || type->native_width == XR_NATIVE_U32 ||
            type->native_width == XR_NATIVE_U64);
}

static XrType *xa_load_le_return_type(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                      const char *label) {
    if (!ctx || !call)
        return xr_type_new_unknown(NULL);
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s expects exactly one type argument", label);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   msg, &loc);
        return xr_type_new_unknown(NULL);
    }
    XrType *target = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    if (!xa_type_is_supported_load_le_result(target)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "%s currently supports T = uint16, uint32 or uint64", label);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
        return xr_type_new_unknown(NULL);
    }
    return target;
}

static XrType *xa_csv_parse_return_type(XaInferContext *ctx, CallExprNode *call) {
    if (!xa_is_module_call(call, "csv", "parse"))
        return NULL;
    XrType *row_array =
        xr_type_new_array(ctx->analyzer->isolate, xr_type_new_string(ctx->analyzer->isolate));
    XrType *rows_array = xr_type_new_array(ctx->analyzer->isolate, row_array);
    if (call->arg_count < 2 || !call->arguments[1])
        return rows_array;
    bool header = false;
    if (xa_object_literal_bool_field(call->arguments[1], "header", &header))
        return header ? xr_type_new_array(ctx->analyzer->isolate,
                                          xr_type_new_json(ctx->analyzer->isolate))
                      : rows_array;
    return NULL;
}

static bool xa_call_object_is_module(XaInferContext *ctx, AstNode *object,
                                     const char *module_name) {
    if (!ctx || !ctx->analyzer || !object || object->type != AST_VARIABLE || !module_name)
        return false;
    const char *name = object->as.variable.name;
    if (!name)
        return false;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
    if (!sym || sym->kind != XA_SYM_MODULE)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    return links && links->module_name && strcmp(links->module_name, module_name) == 0;
}

static const char *xa_math_module_call_member(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || !xa_call_object_is_module(ctx, ma->object, "math"))
        return NULL;
    return ma->name;
}

static bool xa_all_args_are_int(XrType **arg_types, int arg_count) {
    if (!arg_types || arg_count <= 0)
        return false;
    for (int i = 0; i < arg_count; i++) {
        if (!arg_types[i] || !XR_TYPE_IS_INT(arg_types[i]))
            return false;
    }
    return true;
}

static uint8_t xa_call_param_mode(XrType *callee_type, int slot) {
    if (!callee_type || !XR_TYPE_IS_FUNCTION(callee_type) || slot < 0 ||
        slot >= callee_type->function.param_count || !callee_type->function.param_passing_modes)
        return XR_PARAM_VALUE;
    return callee_type->function.param_passing_modes[slot];
}

static const char *xa_call_param_mode_label(uint8_t mode) {
    if (mode == XR_PARAM_IN)
        return "in";
    if (mode == XR_PARAM_REF)
        return "ref";
    return "value";
}

static XaSymbol *xa_call_variable_symbol(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    return xa_lookup_visible_symbol(ctx, expr->as.variable.name);
}

static XaSymbol *xa_call_root_variable_symbol(XaInferContext *ctx, AstNode *expr) {
    while (expr) {
        if (expr->type == AST_VARIABLE)
            return xa_call_variable_symbol(ctx, expr);
        if (expr->type == AST_MEMBER_ACCESS) {
            expr = expr->as.member_access.object;
            continue;
        }
        if (expr->type == AST_INDEX_GET) {
            expr = expr->as.index_get.array;
            continue;
        }
        break;
    }
    return NULL;
}

static void xa_check_ref_argument_not_readonly(XaInferContext *ctx, AstNode *call_node,
                                               AstNode *arg_node, int slot, uint8_t mode) {
    if (!ctx || !ctx->analyzer || mode != XR_PARAM_REF || !arg_node)
        return;
    XaSymbol *root = xa_call_root_variable_symbol(ctx, arg_node);
    if (!root || root->kind != XA_SYM_PARAMETER || root->passing_mode != XR_PARAM_IN)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node->line ? arg_node->line : call_node->line,
                      .column = arg_node->column ? arg_node->column : call_node->column};
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Cannot pass 'in' parameter '%s' to 'ref' parameter %d (readonly reference)",
             root->name ? root->name : "?", slot + 1);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN, msg,
                               &loc);
}

static void xa_check_ref_argument_aliases(XaInferContext *ctx, AstNode *call_node,
                                          uint32_t *arg_symbol_ids, const char **arg_names,
                                          uint8_t *arg_modes, int arg_count) {
    if (!ctx || !ctx->analyzer || !arg_symbol_ids || !arg_names || !arg_modes)
        return;
    for (int i = 0; i < arg_count; i++) {
        if (arg_symbol_ids[i] == 0 || arg_modes[i] == XR_PARAM_VALUE)
            continue;
        for (int j = i + 1; j < arg_count; j++) {
            if (arg_symbol_ids[j] == 0 || arg_modes[j] == XR_PARAM_VALUE)
                continue;
            if (arg_symbol_ids[i] != arg_symbol_ids[j])
                continue;
            if (arg_modes[i] != XR_PARAM_REF && arg_modes[j] != XR_PARAM_REF)
                continue;

            XrLocation loc = {
                .file = ctx->file_path, .line = call_node->line, .column = call_node->column};
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Cannot pass '%s' to both %s parameter %d and %s parameter %d in the same "
                     "call",
                     arg_names[i] ? arg_names[i] : "?", xa_call_param_mode_label(arg_modes[i]),
                     i + 1, xa_call_param_mode_label(arg_modes[j]), j + 1);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
        }
    }
}

static bool xa_method_stores_argument(XrType *receiver_type, const char *method_name, int slot) {
    if (!receiver_type || !method_name || slot < 0)
        return false;
    if (XR_TYPE_IS_ARRAY(receiver_type)) {
        return slot == 0 &&
               (strcmp(method_name, "push") == 0 || strcmp(method_name, "unshift") == 0 ||
                strcmp(method_name, "fill") == 0);
    }
    if (XR_TYPE_IS_MAP(receiver_type))
        return strcmp(method_name, "set") == 0 && (slot == 0 || slot == 1);
    if (receiver_type->kind == XR_KIND_SET)
        return strcmp(method_name, "add") == 0 && slot == 0;
    if (receiver_type->kind == XR_KIND_CHANNEL) {
        return slot == 0 &&
               (strcmp(method_name, "send") == 0 || strcmp(method_name, "trySend") == 0 ||
                strcmp(method_name, "sendTimeout") == 0);
    }
    if (xr_type_is_named_class(receiver_type, "WorkQueue"))
        return strcmp(method_name, "push") == 0 && slot == 0;
    return false;
}

static void xa_check_borrowed_mutator_arg_escape(XaInferContext *ctx, AstNode *call_node,
                                                 XrType *receiver_type, const char *method_name,
                                                 AstNode *arg_node, XrType *arg_type, int slot) {
    if (!ctx || !call_node || !arg_node ||
        !xa_method_stores_argument(receiver_type, method_name, slot))
        return;
    if (!xa_type_needs_borrow_escape_guard(arg_type))
        return;
    XaSymbol *borrowed_root = xa_borrowed_param_root_symbol(ctx, arg_node);
    if (!borrowed_root)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node->line ? arg_node->line : call_node->line,
                      .column = arg_node->column ? arg_node->column : call_node->column};
    const char *mode = borrowed_root->passing_mode == XR_PARAM_REF ? "ref" : "in";
    const char *name = borrowed_root->name ? borrowed_root->name : "?";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot pass borrowed '%s' parameter '%s' to mutating method '%s'; pass an owned "
             "value or copy(%s)",
             mode, name, method_name ? method_name : "?", name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static bool xa_is_channel_send_boundary_method(XrType *receiver_type, const char *method_name) {
    return receiver_type && receiver_type->kind == XR_KIND_CHANNEL && method_name &&
           (strcmp(method_name, "send") == 0 || strcmp(method_name, "trySend") == 0 ||
            strcmp(method_name, "sendTimeout") == 0);
}

static XaSymbol *xa_lookup_visible_class_symbol(XaInferContext *ctx, const char *class_name) {
    if (!ctx || !ctx->analyzer || !class_name)
        return NULL;

    XaSymbol *sym = xa_lookup_visible_symbol(ctx, class_name);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;

    sym = xa_scope_lookup(ctx->analyzer->global_scope, class_name);
    return sym && sym->kind == XA_SYM_CLASS ? sym : NULL;
}

/* Namespace-imported class construction: for a callee shaped as
 * `moduleAlias.ClassName` where `moduleAlias` is an imported module, resolve
 * the exported class from the module graph and return its instance type.
 *
 * This mirrors the bare `ClassName(...)` construction path (which returns
 * xr_type_new_instance from the class symbol's class_info) so a value built via
 * `ns.Class(...)` / `ns.Class<T>(...)` carries its class identity. Without it
 * the call inferred `unknown`, erasing the receiver class for every later
 * `value.method(...)`: generic (map-backed) classes then fell back to dynamic
 * dispatch returning null, and native-struct classes hit a missing boxed-
 * adapter link error in AOT. Returns NULL when the callee is not a module-
 * member class reference, so every other call falls through unchanged. */
static XrType *xa_module_member_class_instance_type(XaInferContext *ctx, AstNode *callee) {
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object || ma->object->type != AST_VARIABLE ||
        !ma->object->as.variable.name)
        return NULL;

    XaSymbol *mod_sym = xa_scope_lookup(ctx->analyzer->current_scope, ma->object->as.variable.name);
    if (!mod_sym || mod_sym->kind != XA_SYM_MODULE)
        return NULL;

    XaSymbolLinks *mod_links = xa_analyzer_get_links(ctx->analyzer, mod_sym);
    const char *mod_name = (mod_links && mod_links->module_name) ? mod_links->module_name
                                                                 : ma->object->as.variable.name;
    bool is_quoted = (mod_name[0] == '.' || mod_name[0] == '/');
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, mod_name, is_quoted);
    if (!exports)
        return NULL;

    XaSymbol *member_sym = (XaSymbol *) xr_hashmap_get(exports, ma->name);
    if (!member_sym || member_sym->kind != XA_SYM_CLASS)
        return NULL;

    XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member_sym);
    if (member_links && member_links->class_info)
        return xr_type_new_instance(ctx->analyzer->isolate, member_links->class_info);
    return NULL;
}

/* Resolve a module-member call's target function-symbol links (namespace.fn),
 * mirroring xa_module_member_class_instance_type but for functions. Used so
 * caller-side default-argument filling applies to `mod.fn(...)` calls exactly
 * as it does to bare `fn(...)` calls. Returns NULL for non-module callees. */
static XaSymbolLinks *xa_module_member_fn_links(XaInferContext *ctx, AstNode *callee) {
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object || ma->object->type != AST_VARIABLE ||
        !ma->object->as.variable.name)
        return NULL;
    XaSymbol *mod_sym = xa_scope_lookup(ctx->analyzer->current_scope, ma->object->as.variable.name);
    if (!mod_sym || mod_sym->kind != XA_SYM_MODULE)
        return NULL;
    XaSymbolLinks *mod_links = xa_analyzer_get_links(ctx->analyzer, mod_sym);
    const char *mod_name = (mod_links && mod_links->module_name) ? mod_links->module_name
                                                                 : ma->object->as.variable.name;
    bool is_quoted = (mod_name[0] == '.' || mod_name[0] == '/');
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, mod_name, is_quoted);
    if (!exports)
        return NULL;
    XaSymbol *member_sym = (XaSymbol *) xr_hashmap_get(exports, ma->name);
    if (!member_sym || member_sym->kind != XA_SYM_FUNCTION)
        return NULL;
    return xa_analyzer_get_links(ctx->analyzer, member_sym);
}

static void xa_check_channel_send_transfer_arg(XaInferContext *ctx, AstNode *call_node,
                                               XrType *receiver_type, const char *method_name,
                                               AstNode *arg_node, XrType *arg_type, int slot) {
    if (slot != 0 || !xa_is_channel_send_boundary_method(receiver_type, method_name))
        return;
    xa_check_boundary_transfer_arg(ctx, call_node, arg_node, arg_type, "channel send argument");
}

static XaSymbolLinks *xa_method_symbol_links_for_call(XaInferContext *ctx, XrType *receiver_type,
                                                      const char *method_name) {
    if (!ctx || !receiver_type || !method_name)
        return NULL;
    const char *class_name = xr_type_get_class_name(receiver_type);
    if (!class_name)
        return NULL;
    XaSymbol *class_sym = xa_lookup_visible_class_symbol(ctx, class_name);
    XaSymbolLinks *class_links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
    XaSymbol *method_sym = (class_links && class_links->class_info)
                               ? xa_class_info_lookup_member(class_links->class_info, method_name)
                               : NULL;
    return method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
}

static void xa_check_borrowed_escaping_param_arg(XaInferContext *ctx, AstNode *call_node,
                                                 XaSymbolLinks *callee_links,
                                                 const char *callee_name, AstNode *arg_node,
                                                 XrType *arg_type, int slot) {
    if (!ctx || !call_node || !callee_links || !callee_links->param_escapes || slot < 0 ||
        slot >= callee_links->param_escape_count || !callee_links->param_escapes[slot])
        return;
    if (!xa_type_needs_borrow_escape_guard(arg_type))
        return;
    XaSymbol *borrowed_root = xa_borrowed_param_root_symbol(ctx, arg_node);
    if (!borrowed_root)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node && arg_node->line ? arg_node->line : call_node->line,
                      .column =
                          arg_node && arg_node->column ? arg_node->column : call_node->column};
    const char *mode = borrowed_root->passing_mode == XR_PARAM_REF ? "ref" : "in";
    const char *name = borrowed_root->name ? borrowed_root->name : "?";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot pass borrowed '%s' parameter '%s' to escaping parameter %d of '%s'; pass an "
             "owned value or copy(%s)",
             mode, name, slot + 1, callee_name ? callee_name : "callee", name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static XrType *xa_math_runtime_shape_return_type(XaInferContext *ctx, CallExprNode *call,
                                                 XrType **arg_types, int arg_count) {
    const char *member = xa_math_module_call_member(ctx, call);
    if (!member)
        return NULL;
    if (strcmp(member, "abs") == 0 && arg_count == 1 && xa_all_args_are_int(arg_types, 1))
        return xr_type_new_unknown(ctx && ctx->analyzer ? ctx->analyzer->isolate : NULL);
    if ((strcmp(member, "min") == 0 || strcmp(member, "max") == 0) && arg_count == 0)
        return xr_type_new_unknown(ctx && ctx->analyzer ? ctx->analyzer->isolate : NULL);
    if ((strcmp(member, "min") == 0 || strcmp(member, "max") == 0) &&
        xa_all_args_are_int(arg_types, arg_count))
        return xr_type_new_int(NULL);
    if (strcmp(member, "clamp") == 0 && arg_count == 3 && xa_all_args_are_int(arg_types, 3))
        return xr_type_new_int(NULL);
    return NULL;
}

/* ----------------------------------------------------------------------------
 * Function Call Type Inference
 * Handles: argument count/type checking, generic type argument validation,
 * type parameter inference for generic functions, and callback type inference
 * for container methods (map, filter, reduce, etc.)
 * -------------------------------------------------------------------------- */
XrType *xa_visit_call(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    CallExprNode *call = &node->as.call_expr;
    bool optional_function_call = call->callee && call->callee->type == AST_OPTIONAL_CHAIN &&
                                  call->callee->as.optional_chain.chain_type == 3;

    if (xa_call_is_sys_thread_spawn(call))
        return xa_visit_sys_thread_spawn_call(ctx, node, call);

    // Record dependency: current function depends on called function
    XaSymbol *fn_sym = NULL;
    XaSymbolLinks *fn_links = NULL;

    if (call->callee && call->callee->type == AST_VARIABLE) {
        const char *fn_name = call->callee->as.variable.name;
        fn_sym = xa_lookup_visible_symbol(ctx, fn_name);
        if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION) {
            fn_links = xa_analyzer_get_links(ctx->analyzer, fn_sym);

            // FFI: calling an @extern function is unsafe — it crosses into a
            // foreign C ABI with no Xray safety guarantees. Permit it only
            // inside an `unsafe { }` region (Rust model).
            if (fn_links && fn_links->is_extern && ctx->unsafe_depth == 0) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "call to extern function '%s' must be inside an `unsafe { }` block",
                         fn_name ? fn_name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
            }

            if (ctx->current_function && ctx->analyzer->incremental) {
                XaIncrementalCtx *incr = (XaIncrementalCtx *) ctx->analyzer->incremental;
                xa_dep_add(incr, ctx->current_function->id, fn_sym->id, XA_DEP_CALL);
            }
        }
    } else if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        // Module member call (namespace.fn): resolve the exported function's
        // links so caller-side default-argument filling below applies just as
        // for a bare-name call. Non-module member calls (instance methods)
        // resolve to NULL here and are unaffected.
        fn_links = xa_module_member_fn_links(ctx, call->callee);
    }

    // Check generic type argument count and constraints
    if (call->type_arg_count > 0 && fn_links) {
        int expected_count = xa_symbol_links_get_type_param_count(fn_links);

        // Check count matches
        if (call->type_arg_count != expected_count) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            const char *fn_name = call->callee && call->callee->type == AST_VARIABLE
                                      ? call->callee->as.variable.name
                                      : "function";
            snprintf(msg, sizeof(msg),
                     "Generic function '%s' expects %d type argument(s), but got %d", fn_name,
                     expected_count, call->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
        }

        // Check constraints — every constraint in the intersection list must hold.
        for (int i = 0; i < call->type_arg_count && i < expected_count; i++) {
            // Use analyzer-aware resolver so user class type-args carry their
            // superclass chain — required for `<T: BaseClass>` upper bounds.
            XrType *type_arg = call->type_args[i]
                                   ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[i])
                                   : NULL;

            int constraint_count = 0;
            XrType **constraints =
                xa_symbol_links_get_type_param_constraints(fn_links, i, &constraint_count);

            if (!type_arg || constraint_count == 0)
                continue;

            for (int j = 0; j < constraint_count; j++) {
                XrType *constraint = constraints[j];
                if (constraint && !xr_type_satisfies_constraint(type_arg, constraint)) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    const char *param_name = xa_symbol_links_get_type_param_name(fn_links, i);
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Type '%s' does not satisfy constraint '%s' for type parameter '%s'",
                             xr_type_to_string(type_arg), xr_type_to_string(constraint),
                             param_name ? param_name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
                }
            }
        }
    } else if (fn_links && xa_symbol_links_get_type_param_count(fn_links) > 0 && call->callee &&
               call->callee->type == AST_VARIABLE) {
        // Implicit generic instantiation: type args inferred from arguments.
        // For each parameter typed as a bare T, infer T = type(arg) and
        // verify constraints on T.  This mirrors the explicit branch above
        // but does its own simple inference per type parameter.
        XaSymbol *fn_sym = xa_lookup_visible_symbol(ctx, call->callee->as.variable.name);
        if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION) {
            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, fn_sym);
            int tp_count = xa_symbol_links_get_type_param_count(fl);
            int p_count = fl ? fl->param_count : 0;
            for (int ti = 0; ti < tp_count; ti++) {
                int constraint_count = 0;
                XrType **constraints =
                    xa_symbol_links_get_type_param_constraints(fl, ti, &constraint_count);
                if (constraint_count == 0)
                    continue;
                const char *tp_name = xa_symbol_links_get_type_param_name(fl, ti);
                if (!tp_name)
                    continue;
                // Find the first parameter whose type is exactly the bare T
                XrType *inferred = NULL;
                for (int pi = 0; pi < p_count && pi < call->arg_count; pi++) {
                    XrType *pt = fl->param_types ? fl->param_types[pi] : NULL;
                    if (!pt || pt->kind != XR_KIND_TYPE_PARAM)
                        continue;
                    if (!pt->type_param.name || strcmp(pt->type_param.name, tp_name) != 0)
                        continue;
                    AstNode *a = call->arguments[pi];
                    if (a && a->type != AST_FUNCTION_EXPR)
                        inferred = xa_visit_infer_expr(ctx, a);
                    if (inferred)
                        break;
                }
                if (!inferred || XR_TYPE_IS_UNKNOWN(inferred))
                    continue;
                for (int j = 0; j < constraint_count; j++) {
                    XrType *constraint = constraints[j];
                    if (constraint && !xr_type_satisfies_constraint(inferred, constraint)) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[256];
                        snprintf(
                            msg, sizeof(msg),
                            "Type '%s' does not satisfy constraint '%s' for type parameter '%s'",
                            xr_type_to_string(inferred), xr_type_to_string(constraint), tp_name);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
                    }
                }
            }
        }
    }

    // Recognize Json.decode<T>(data): compiler-generated typed decode
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS && call->type_arg_count == 1) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        if (ma->name && strcmp(ma->name, "decode") == 0 && ma->object &&
            ma->object->type == AST_VARIABLE && strcmp(ma->object->as.variable.name, "Json") == 0) {
            XrType *target_type = call->type_args[0]
                                      ? xr_tref_resolve(ctx->analyzer->isolate, call->type_args[0])
                                      : NULL;

            // Resolve type alias to its underlying object type
            if (target_type && target_type->kind == XR_KIND_CLASS &&
                target_type->instance.class_name) {
                XaSymbol *alias_sym =
                    xa_scope_lookup(ctx->analyzer->current_scope, target_type->instance.class_name);
                if (alias_sym && alias_sym->kind == XA_SYM_TYPE_ALIAS && alias_sym->alias_type) {
                    target_type = alias_sym->alias_type;
                }
            }

            // Validate: target must be a sealed Record type with known fields.
            if (!target_type || !XR_TYPE_IS_RECORD(target_type) || !target_type->object.is_sealed ||
                target_type->object.field_count == 0) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_CONSTRAINT,
                    "Json.decode<T>() requires T to be a sealed Record type alias with fields",
                    &loc);
                return xr_type_new_unknown(NULL);
            }

            // Validate: exactly 1 argument
            if (call->arg_count != 1) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE,
                                           "Json.decode<T>() expects exactly 1 argument", &loc);
                return xr_type_new_unknown(NULL);
            }

            // Visit argument to ensure it's analyzed
            xa_visit_infer_expr(ctx, call->arguments[0]);

            // Return T? (decode can fail, returning null)
            XrType *result = xr_type_copy(ctx->analyzer->isolate, target_type);
            if (result)
                result->is_nullable = true;
            return result;
        }
    }

    // Check for method call pattern: container.method(callback)
    // to enable generic type inference for callbacks
    XrType *container_elem_type = NULL;
    const char *method_name = NULL;
    XrType *callee_obj_type = NULL;  // cached once; reused by filter/reduce/generic substitution

    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        method_name = ma->name;
        callee_obj_type = xa_call_raw_pointer_type_namespace(ctx, ma->object);
        if (!callee_obj_type)
            callee_obj_type = xa_visit_infer_expr(ctx, ma->object);

        // Enforce private/protected visibility on user-class method calls.
        if (method_name && callee_obj_type && XR_TYPE_IS_INSTANCE(callee_obj_type) &&
            callee_obj_type->instance.class_name) {
            XaSymbol *mc_sym =
                xa_scope_lookup(ctx->analyzer->current_scope, callee_obj_type->instance.class_name);
            XaSymbolLinks *mc_links = mc_sym ? xa_analyzer_get_links(ctx->analyzer, mc_sym) : NULL;
            if (mc_links && mc_links->class_info) {
                struct XrClassInfo *m_owner = NULL;
                XaSymbol *m_sym = xa_class_info_lookup_instance_member_owner(mc_links->class_info,
                                                                             method_name, &m_owner);
                if (m_sym && m_sym->kind == XA_SYM_METHOD)
                    xa_check_member_visibility(ctx, call->callee, m_sym, m_owner);
            }
        }

        XaSymbol *in_param = xa_in_param_symbol_for_expr(ctx, ma->object);
        if (in_param && method_name) {
            // Decide whether the call mutates the `in` receiver. For user class
            // instances use the precise body-derived flag (mutates_receiver); the
            // method-name heuristic only applies to builtin containers/strings,
            // which have no analyzable body. Applying the name list to instances
            // would wrongly reject immutable value-style APIs (e.g. a read-only
            // `Vec.add` that returns a new value without touching `this`).
            bool call_mutates_receiver;
            const char *recv_class_name =
                callee_obj_type ? xr_type_get_class_name(callee_obj_type) : NULL;
            if (recv_class_name) {
                XaSymbol *class_sym = xa_lookup_visible_class_symbol(ctx, recv_class_name);
                XaSymbolLinks *class_links =
                    class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
                XaSymbol *method_sym =
                    (class_links && class_links->class_info)
                        ? xa_class_info_lookup_instance_member(class_links->class_info, method_name)
                        : NULL;
                call_mutates_receiver =
                    method_sym && method_sym->kind == XA_SYM_METHOD && method_sym->mutates_receiver;
            } else {
                call_mutates_receiver = xa_method_name_mutates_receiver(method_name);
            }
            if (call_mutates_receiver) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "Cannot call mutating method '%s' on 'in' parameter '%s' (readonly "
                         "reference)",
                         method_name, in_param->name ? in_param->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
        }

        // Extract element type from container
        if (XR_TYPE_IS_ARRAY(callee_obj_type) && callee_obj_type->container.element_type) {
            container_elem_type = callee_obj_type->container.element_type;
        } else if ((callee_obj_type->kind == XR_KIND_SET) &&
                   callee_obj_type->container.element_type) {
            container_elem_type = callee_obj_type->container.element_type;
        } else if (XR_TYPE_IS_MAP(callee_obj_type)) {
            // For Map, callback gets (value, key) or (key, value) depending on method
            if (method_name && strcmp(method_name, "forEach") == 0) {
                container_elem_type = callee_obj_type->map.value_type;
            }
        }
    }

    XrType *callee_type = xa_visit_infer_expr(ctx, call->callee);
    if (optional_function_call && callee_type)
        callee_type = xr_type_non_nullable(ctx->analyzer->isolate, callee_type);

    /* Resolve symbol_ids in non-lambda arguments before any early-return path.
     * Skip AST_FUNCTION_EXPR args: they require expected_type context from
     * the callee's parameter signature (set in the detailed loop below).
     * Visiting them eagerly without context triggers spurious E0365. */
    for (int i = 0; i < call->arg_count; i++) {
        if (call->arguments[i] && call->arguments[i]->type != AST_FUNCTION_EXPR)
            xa_visit_infer_expr(ctx, call->arguments[i]);
    }

    /* Namespace-imported class construction (`ns.Class(...)` / `ns.Class<T>(...)`)
     * resolves to the class instance type, exactly like the bare `Class(...)`
     * path. Done before the unknown-callee/class-callee branches below, which
     * only recognise AST_VARIABLE callees, so the receiver keeps its class
     * identity for cross-module method resolution and AOT codegen. */
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        XrType *ns_instance = xa_module_member_class_instance_type(ctx, call->callee);
        if (ns_instance)
            return ns_instance;
    }

    // Unknown callee type preserves error recovery after imprecise analysis.
    if (XR_TYPE_IS_UNKNOWN(callee_type)) {
        // Check if callee is a class name - if so, return instance type
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *name = call->callee->as.variable.name;

            /* Atomic(expr): infer Atomic<T> from argument type */
            if (strcmp(name, "Atomic") == 0) {
                XrType *et = NULL;
                if (call->arg_count > 0 && call->arguments[0])
                    et = xa_visit_infer_expr(ctx, call->arguments[0]);
                if (!et)
                    et = xr_type_new_unknown(NULL);
                XrType **arg_copy = (XrType **) xr_malloc(sizeof(XrType *));
                if (arg_copy) {
                    arg_copy[0] = et;
                    return xr_type_new_generic_instance(ctx->analyzer->isolate, "Atomic", NULL,
                                                        arg_copy, 1);
                }
            }
            if (strcmp(name, "WorkQueue") == 0) {
                XrType *et = NULL;
                if (call->type_arg_count > 0 && call->type_args[0])
                    et = xr_tref_resolve(ctx->analyzer->isolate, call->type_args[0]);
                if (!et)
                    et = xr_type_new_unknown(NULL);
                XrType **arg_copy = (XrType **) xr_malloc(sizeof(XrType *));
                if (arg_copy) {
                    arg_copy[0] = et;
                    return xr_type_new_generic_instance(ctx->analyzer->isolate, "WorkQueue", NULL,
                                                        arg_copy, 1);
                }
            }
            if (strcmp(name, "ResultGroup") == 0) {
                return xr_type_new_named_instance(ctx->analyzer->isolate, "ResultGroup");
            }
            if (strcmp(name, "CountdownLatch") == 0) {
                return xr_type_new_named_instance(ctx->analyzer->isolate, "CountdownLatch");
            }
            if (strcmp(name, "Semaphore") == 0) {
                return xr_type_new_named_instance(ctx->analyzer->isolate, "Semaphore");
            }
            if (strcmp(name, "EventCount") == 0) {
                return xr_type_new_named_instance(ctx->analyzer->isolate, "EventCount");
            }
            if (strcmp(name, "Thread") == 0) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                    "Thread handles can only be created by sys.Thread.spawn", &loc);
                return xr_type_new_unknown(NULL);
            }

            // Construction `T(args)`: resolve the class in any visible scope
            // (global, enclosing function for nested classes). new-expr used to
            // be the only path that handled non-global classes; unify here.
            XaSymbol *sym = xa_lookup_visible_symbol(ctx, name);
            if (!sym || sym->kind != XA_SYM_CLASS)
                sym = xa_scope_lookup(ctx->analyzer->global_scope, name);
            if (sym && sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links && links->class_info) {
                    return xr_type_new_instance(ctx->analyzer->isolate, links->class_info);
                }
            }

            // Built-in primitive class Exception (and bare construction of it):
            // `Exception(msg)` constructs the runtime exception instance.
            if (strcmp(name, "PanicInfo") == 0) {
                return xr_type_new_named_instance(ctx->analyzer->isolate, "PanicInfo");
            }
        }
        // Container method with callback: infer fn expr arg types even though
        // the method's own return type resolved to unknown (e.g. reduce).
        if (container_elem_type && method_name) {
            XrType *saved_elem = ctx->callback_element_type;
            XrType *saved_idx = ctx->callback_index_type;
            XrType *saved_acc = ctx->callback_accumulator_type;
            XrType *saved_arr = ctx->callback_array_type;
            ctx->callback_element_type = container_elem_type;
            ctx->callback_index_type = xr_type_new_int(NULL);
            if (strcmp(method_name, "reduce") == 0 && call->arg_count >= 2 && call->arguments[1]) {
                ctx->callback_accumulator_type = xa_visit_infer_expr(ctx, call->arguments[1]);
                ctx->callback_array_type = callee_obj_type;
            }
            for (int i = 0; i < call->arg_count; i++) {
                if (call->arguments[i])
                    xa_visit_infer_expr(ctx, call->arguments[i]);
            }
            ctx->callback_element_type = saved_elem;
            ctx->callback_index_type = saved_idx;
            ctx->callback_accumulator_type = saved_acc;
            ctx->callback_array_type = saved_arr;
        }
        return xr_type_new_unknown(NULL);
    }

    // Class constructor call: ClassName(args) returns instance type
    if (callee_type->kind == XR_KIND_CLASS) {
        // Look up class symbol to get XrClassInfo. Try the current scope first
        // (covers function-local / nested class declarations) then global.
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *class_name = call->callee->as.variable.name;
            if (strcmp(class_name, "Thread") == 0) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                    "Thread handles can only be created by sys.Thread.spawn", &loc);
                return xr_type_new_unknown(NULL);
            }
            XaSymbol *class_sym = xa_lookup_visible_symbol(ctx, class_name);
            if (!class_sym || class_sym->kind != XA_SYM_CLASS)
                class_sym = xa_scope_lookup(ctx->analyzer->global_scope, class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (links && links->class_info) {
                    return xr_type_new_instance(ctx->analyzer->isolate, links->class_info);
                }
            }
        }
        return xr_type_new_unknown(NULL);
    }

    // Union of function types: method dispatch on union (e.g. shape.area()
    // where shape: Circle | Rect). Each union member resolves to a function
    // type; extract each return type and union them.
    if (XR_TYPE_IS_UNION(callee_type)) {
        XrType *ret_union = NULL;
        bool all_functions = true;
        for (int i = 0; i < callee_type->union_type.member_count; i++) {
            XrType *m = callee_type->union_type.members[i];
            if (!m || !XR_TYPE_IS_FUNCTION(m)) {
                all_functions = false;
                break;
            }
            XrType *rt = m->function.return_type;
            if (!rt)
                rt = xr_type_new_unknown(NULL);
            ret_union = ret_union ? xr_type_union(ctx->analyzer->isolate, ret_union, rt) : rt;
        }
        if (all_functions && ret_union)
            return ret_union;
    }

    // Check if callee is callable
    if (!XR_TYPE_IS_FUNCTION(callee_type)) {
        // Builtin method call: container.method() where member_access returned
        // the method's return type directly (e.g. arr.length() → int).
        // Accept primitive/container return types without warning.
        if (call->callee && call->callee->type == AST_MEMBER_ACCESS && callee_type &&
            !XR_TYPE_IS_UNKNOWN(callee_type)) {
            return callee_type;
        }
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "Value is not callable", &loc);
        return xr_type_new_unknown(NULL);
    }

    // Infer argument types
    int param_count = callee_type->function.param_count;
    bool is_variadic = callee_type->function.is_variadic;

    // Caller-side default argument filling (C1): for a direct call to a named
    // function with default parameters, complete omitted trailing arguments by
    // appending session-cloned copies of the declared default expressions. This
    // makes defaults evaluated at the call site instead of via a runtime null
    // sentinel, so passing an explicit `null` is preserved (not treated as
    // omitted). Indirect/function-value calls carry no default expressions and
    // therefore must pass every argument.
    if (fn_links && fn_links->param_defaults && !is_variadic &&
        fn_links->param_count == param_count && call->arg_count < param_count) {
        bool can_complete = true;
        for (int i = 0; i < call->arg_count; i++) {
            if (call->arguments[i] && call->arguments[i]->type == AST_SPREAD_EXPR) {
                can_complete = false;
                break;
            }
        }
        for (int i = call->arg_count; can_complete && i < param_count; i++) {
            if (!fn_links->param_defaults[i])
                can_complete = false;  // missing required arg → real arity error below
        }
        if (can_complete) {
            XrCompilerSession *sess =
                ctx->analyzer ? xr_compiler_session_current_for_isolate(ctx->analyzer->isolate)
                              : NULL;
            AstNode **new_args = (AstNode **) xr_calloc((size_t) param_count, sizeof(AstNode *));
            if (new_args) {
                for (int i = 0; i < call->arg_count; i++)
                    new_args[i] = call->arguments[i];
                for (int i = call->arg_count; i < param_count; i++)
                    new_args[i] = xr_ast_clone_session(fn_links->param_defaults[i], sess);
                call->arguments = new_args;
                call->arg_count = param_count;
            }
        }
    }

    /* Spread expansion: walk arguments once, building a flat per-slot
     * view that splices each `...tuple` arg into its individual element
     * slots. With no spreads this is just call->arguments / call->arg_count. */
    int eff_count = 0;
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *a = call->arguments[i];
        if (a && a->type == AST_SPREAD_EXPR) {
            XrType *src = xa_visit_infer_expr(ctx, a->as.spread_expr.expr);
            if (src && XR_TYPE_IS_TUPLE(src)) {
                eff_count += src->tuple.element_count;
            } else {
                XrLocation loc = {.file = ctx->file_path, .line = a->line, .column = a->column};
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Spread '...' argument must be a tuple of statically known arity, "
                         "got '%s'",
                         src ? xr_type_to_string(src) : "<unknown>");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        } else {
            eff_count++;
        }
    }
    int arg_count = eff_count;
    XrType **effective_arg_types = NULL;
    if (arg_count > 0)
        effective_arg_types = (XrType **) xr_calloc((size_t) arg_count, sizeof(XrType *));
    uint32_t *effective_arg_symbol_ids = NULL;
    const char **effective_arg_names = NULL;
    uint8_t *effective_arg_modes = NULL;
    if (arg_count > 0) {
        effective_arg_symbol_ids = (uint32_t *) xr_calloc((size_t) arg_count, sizeof(uint32_t));
        effective_arg_names = (const char **) xr_calloc((size_t) arg_count, sizeof(const char *));
        effective_arg_modes = (uint8_t *) xr_calloc((size_t) arg_count, sizeof(uint8_t));
    }

    // Check argument count (use min_params for functions with default parameters)
    int min_params = callee_type->function.min_params;
    if (arg_count < min_params && !is_variadic) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        if (min_params == param_count) {
            snprintf(msg, sizeof(msg), "Expected %d argument(s), but got %d", param_count,
                     arg_count);
        } else {
            snprintf(msg, sizeof(msg), "Expected %d to %d argument(s), but got %d", min_params,
                     param_count, arg_count);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
    } else if (arg_count > param_count && !is_variadic) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Expected %d argument(s), but got %d", param_count, arg_count);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
    } else if (!is_variadic && arg_count < param_count && min_params < param_count && !fn_links &&
               call->callee && call->callee->type == AST_VARIABLE) {
        // Default arguments are filled at the call site only for direct calls
        // to a named function (C1). A call through a function-typed *value*
        // carries no default expressions, so every argument must be passed.
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Expected %d argument(s), but got %d; default arguments apply only to direct "
                 "calls, not calls through a function value",
                 param_count, arg_count);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
    }

    // Check argument types with generic inference for callbacks
    XrType **param_types = callee_type->function.param_types;

    // Save and set callback context for generic inference
    XrType *saved_elem_type = ctx->callback_element_type;
    XrType *saved_index_type = ctx->callback_index_type;
    XrType *saved_acc_type = ctx->callback_accumulator_type;
    XrType *saved_arr_type = ctx->callback_array_type;

    // Set callback context if this is a container method with callbacks
    if (container_elem_type && method_name) {
        // Methods that take callbacks: map, filter, forEach, find, etc.
        if (strcmp(method_name, "map") == 0 || strcmp(method_name, "filter") == 0 ||
            strcmp(method_name, "forEach") == 0 || strcmp(method_name, "find") == 0 ||
            strcmp(method_name, "findIndex") == 0 || strcmp(method_name, "every") == 0 ||
            strcmp(method_name, "some") == 0) {
            ctx->callback_element_type = container_elem_type;
            ctx->callback_index_type = xr_type_new_int(NULL);
        }
        // reduce: fn(acc, item, index, array) => acc
        else if (strcmp(method_name, "reduce") == 0) {
            ctx->callback_element_type = container_elem_type;
            ctx->callback_index_type = xr_type_new_int(NULL);
            // Get accumulator type from second argument (initial value)
            if (arg_count >= 2 && call->arguments[1]) {
                ctx->callback_accumulator_type = xa_visit_infer_expr(ctx, call->arguments[1]);
            }
            // Store array type for 4th callback param
            ctx->callback_array_type = callee_obj_type;
        }
    }

    /* Effective-slot iteration: a spread `...t` arg contributes one
     * slot per element of the source tuple. Each slot is checked
     * against the next parameter; non-tuple spread sources contribute
     * zero slots (the diagnostic was already emitted above). */
    int slot = 0;
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *arg_node = call->arguments[i];
        if (!arg_node)
            continue;

        if (arg_node->type == AST_SPREAD_EXPR) {
            /* xa_visit_infer_expr was already called above for the
             * count check; re-querying gives the cached node type. */
            XrType *src = xa_analyzer_get_node_type(ctx->analyzer, arg_node->as.spread_expr.expr);
            if (!src) {
                XrType *saved_expected = ctx->expected_type;
                ctx->expected_type = NULL;
                src = xa_visit_infer_expr(ctx, arg_node->as.spread_expr.expr);
                ctx->expected_type = saved_expected;
            }
            if (!src || !XR_TYPE_IS_TUPLE(src))
                continue;
            for (int j = 0; j < src->tuple.element_count; j++, slot++) {
                XrType *arg_type = src->tuple.element_types[j];
                if (effective_arg_types && slot < arg_count)
                    effective_arg_types[slot] = arg_type;
                if (effective_arg_modes && slot < arg_count)
                    effective_arg_modes[slot] = xa_call_param_mode(callee_type, slot);
                xa_check_channel_send_transfer_arg(ctx, node, callee_obj_type, method_name,
                                                   arg_node, arg_type, slot);
                if (slot >= param_count)
                    continue;
                XrType *param_type = param_types ? param_types[slot] : NULL;
                if (!param_type || XR_TYPE_IS_UNKNOWN(param_type))
                    continue;
                if (xa_type_is_c_callback(param_type)) {
                    xa_report_c_callback_requires_top_level(ctx, arg_node, slot);
                    continue;
                }
                XrLocation loc = {
                    .file = ctx->file_path, .line = arg_node->line, .column = arg_node->column};
                bool null_err =
                    xa_check_null_safety(ctx->analyzer, param_type, arg_type, "Argument", &loc);
                if (!null_err && !xa_typecheck_assignable(param_type, arg_type) &&
                    !xr_is_json_coercion(param_type, arg_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Argument %d (from spread): type '%s' is not assignable to "
                             "parameter type '%s'",
                             slot + 1, xr_type_to_string(arg_type), xr_type_to_string(param_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
                }
            }
            continue;
        }

        if (slot >= param_count) {
            XrType *arg_type = xa_visit_infer_expr(ctx, arg_node);
            if (effective_arg_types && slot < arg_count)
                effective_arg_types[slot] = arg_type;
            if (effective_arg_modes && slot < arg_count)
                effective_arg_modes[slot] = xa_call_param_mode(callee_type, slot);
            slot++;
            continue;
        }

        XrType *param_type = param_types ? param_types[slot] : NULL;
        XrType *saved_expected = ctx->expected_type;
        if (param_type && !XR_TYPE_IS_UNKNOWN(param_type)) {
            ctx->expected_type = param_type;
        }
        XrType *arg_type = xa_visit_infer_expr(ctx, arg_node);
        ctx->expected_type = saved_expected;
        if (effective_arg_types && slot < arg_count)
            effective_arg_types[slot] = arg_type;
        xa_check_channel_send_transfer_arg(ctx, node, callee_obj_type, method_name, arg_node,
                                           arg_type, slot);
        uint8_t param_mode = xa_call_param_mode(callee_type, slot);
        if (effective_arg_modes && slot < arg_count)
            effective_arg_modes[slot] = param_mode;
        xa_check_ref_argument_not_readonly(ctx, node, arg_node, slot, param_mode);
        if (param_mode == XR_PARAM_IN || param_mode == XR_PARAM_REF) {
            XaSymbol *arg_sym = xa_call_variable_symbol(ctx, arg_node);
            if (arg_sym && effective_arg_symbol_ids && effective_arg_names && slot < arg_count) {
                effective_arg_symbol_ids[slot] = arg_sym->id;
                effective_arg_names[slot] = arg_sym->name;
            }
        }

        if (param_type && !XR_TYPE_IS_UNKNOWN(param_type)) {
            if (xa_type_is_c_callback(param_type)) {
                if (!xa_c_callback_arg_is_top_level_function(ctx, arg_node)) {
                    xa_report_c_callback_requires_top_level(ctx, arg_node, slot);
                } else if (!xa_c_callback_signature_matches(param_type, arg_type)) {
                    xa_report_c_callback_signature_mismatch(ctx, arg_node, slot, param_type,
                                                            arg_type);
                }
                slot++;
                continue;
            }
            XrLocation loc = {
                .file = ctx->file_path, .line = arg_node->line, .column = arg_node->column};
            bool null_err =
                xa_check_null_safety(ctx->analyzer, param_type, arg_type, "Argument", &loc);
            if (!null_err && !xa_typecheck_assignable(param_type, arg_type) &&
                !xr_is_json_coercion(param_type, arg_type)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Argument %d: type '%s' is not assignable to parameter type '%s'",
                         slot + 1, xr_type_to_string(arg_type), xr_type_to_string(param_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
            }
        }
        slot++;
    }
    xa_check_ref_argument_aliases(ctx, node, effective_arg_symbol_ids, effective_arg_names,
                                  effective_arg_modes, arg_count);
    XaSymbolLinks *escape_links = fn_links;
    const char *escape_name = NULL;
    if (call->callee && call->callee->type == AST_VARIABLE)
        escape_name = call->callee->as.variable.name;
    if (!escape_links && method_name && callee_obj_type) {
        escape_links = xa_method_symbol_links_for_call(ctx, callee_obj_type, method_name);
        escape_name = method_name;
    }
    if (escape_links && call->arg_count > 0) {
        int direct_slot = 0;
        for (int i = 0; i < call->arg_count; i++) {
            AstNode *arg_node = call->arguments[i];
            if (!arg_node)
                continue;
            if (arg_node->type == AST_SPREAD_EXPR) {
                XrType *src =
                    xa_analyzer_get_node_type(ctx->analyzer, arg_node->as.spread_expr.expr);
                if (!src)
                    src = xa_visit_infer_expr(ctx, arg_node->as.spread_expr.expr);
                if (src && XR_TYPE_IS_TUPLE(src))
                    direct_slot += src->tuple.element_count;
                continue;
            }
            XrType *arg_type = (direct_slot >= 0 && direct_slot < arg_count)
                                   ? effective_arg_types[direct_slot]
                                   : xa_visit_infer_expr(ctx, arg_node);
            xa_check_borrowed_escaping_param_arg(ctx, node, escape_links, escape_name, arg_node,
                                                 arg_type, direct_slot);
            direct_slot++;
        }
    }
    if (method_name && callee_obj_type && call->arg_count > 0) {
        int direct_slot = 0;
        for (int i = 0; i < call->arg_count; i++) {
            AstNode *arg_node = call->arguments[i];
            if (!arg_node)
                continue;
            if (arg_node->type == AST_SPREAD_EXPR) {
                XrType *src =
                    xa_analyzer_get_node_type(ctx->analyzer, arg_node->as.spread_expr.expr);
                if (!src)
                    src = xa_visit_infer_expr(ctx, arg_node->as.spread_expr.expr);
                if (src && XR_TYPE_IS_TUPLE(src))
                    direct_slot += src->tuple.element_count;
                continue;
            }
            XrType *arg_type = (direct_slot >= 0 && direct_slot < arg_count)
                                   ? effective_arg_types[direct_slot]
                                   : xa_visit_infer_expr(ctx, arg_node);
            xa_check_borrowed_mutator_arg_escape(ctx, node, callee_obj_type, method_name, arg_node,
                                                 arg_type, direct_slot);
            direct_slot++;
        }
    }

    // Restore callback context
    ctx->callback_element_type = saved_elem_type;
    ctx->callback_index_type = saved_index_type;
    ctx->callback_accumulator_type = saved_acc_type;
    ctx->callback_array_type = saved_arr_type;

    // Call-site parameter type propagation: for unannotated parameters,
    // record the inferred argument type so codegen can use it for optimization.
    if (fn_links && arg_count > 0) {
        // Ensure inferred_param_types array is allocated
        if (!fn_links->inferred_param_types && fn_links->param_count > 0) {
            fn_links->inferred_param_types = xr_calloc(fn_links->param_count, sizeof(XrType *));
            if (fn_links->inferred_param_types)
                fn_links->inferred_param_count = fn_links->param_count;
        }
        for (int i = 0; i < arg_count && i < fn_links->inferred_param_count; i++) {
            // Only propagate for unannotated params (declared_type is NULL or unknown)
            XrType *declared = (fn_links->param_types && i < fn_links->param_count)
                                   ? fn_links->param_types[i]
                                   : NULL;
            if (declared && !XR_TYPE_IS_UNKNOWN(declared))
                continue;  // explicitly typed

            XrType *arg_type = effective_arg_types ? effective_arg_types[i] : NULL;
            if (!arg_type || XR_TYPE_IS_UNKNOWN(arg_type))
                continue;

            XrType **slot = &fn_links->inferred_param_types[i];
            if (!*slot) {
                *slot = arg_type;  // First observation
            } else if (!xr_type_equals(*slot, arg_type)) {
                // If types differ only in nullability, widen to nullable
                XrType *a = xr_type_non_nullable(ctx->analyzer->isolate, *slot);
                XrType *b = xr_type_non_nullable(ctx->analyzer->isolate, arg_type);
                if (a && b && xr_type_equals(a, b)) {
                    *slot = xr_type_make_nullable(ctx->analyzer->isolate, a);
                } else {
                    *slot = xr_type_new_unknown(NULL);  // Incompatible callers
                }
            }
        }
    }

    XrType *return_type = callee_type->function.return_type;

    // G2: Override return type for container methods using callback return type
    if (container_elem_type && method_name && arg_count >= 1) {
        if (strcmp(method_name, "map") == 0) {
            // arr.map(fn) -> Array<callback_return_type>
            // Use cached type from argument evaluation above (avoid re-evaluation
            // which would lose callback context)
            // Read callback type from the analyzer side table.
            XrType *cb_type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[0]);
            if (cb_type && XR_TYPE_IS_FUNCTION(cb_type) && cb_type->function.return_type &&
                !XR_TYPE_IS_UNKNOWN(cb_type->function.return_type)) {
                return_type =
                    xr_type_new_array(ctx->analyzer->isolate, cb_type->function.return_type);
            }
        } else if (strcmp(method_name, "filter") == 0) {
            // arr.filter(fn) -> same Array type as source
            if (XR_TYPE_IS_ARRAY(callee_obj_type)) {
                return_type = callee_obj_type;
            }
        } else if (strcmp(method_name, "reduce") == 0 && arg_count >= 2) {
            // arr.reduce(fn, init) -> type of init value
            XrType *init_type = effective_arg_types ? effective_arg_types[1] : NULL;
            if (!init_type && call->arg_count >= 2)
                init_type = xa_visit_infer_expr(ctx, call->arguments[1]);
            if (init_type && !XR_TYPE_IS_UNKNOWN(init_type)) {
                return_type = init_type;
            }
        } else if (strcmp(method_name, "find") == 0) {
            // arr.find(fn) -> element_type? (nullable)
            return_type = xr_type_make_nullable(
                ctx->analyzer->isolate, xr_type_copy(ctx->analyzer->isolate, container_elem_type));
        } else if (strcmp(method_name, "findIndex") == 0) {
            return_type = xr_type_new_int(NULL);
        } else if (strcmp(method_name, "every") == 0 || strcmp(method_name, "some") == 0) {
            return_type = xr_type_new_bool(NULL);
        }
    }

    // Apply type substitution for generic function calls
    if (return_type && fn_links) {
        return_type = xa_substitute_generic_call(ctx, fn_links, callee_type, return_type, call,
                                                 arg_count, effective_arg_types);
    }

    XrType *builtin_override = xa_csv_parse_return_type(ctx, call);
    if (builtin_override)
        return_type = builtin_override;

    XrType *math_shape_type =
        xa_math_runtime_shape_return_type(ctx, call, effective_arg_types, arg_count);
    if (math_shape_type)
        return_type = math_shape_type;

    /* copy(x): the deep-copy builtin is an identity over types and must
     * preserve the argument's static type. It is registered as any->any, so
     * without this the result is unknown and cannot be passed to a typed
     * parameter (breaks `shared const c = copy(obj)` feeding a typed callee).
     * Guard on unknown so a user-defined copy with a concrete signature wins. */
    if (call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
        strcmp(call->callee->as.variable.name, "copy") == 0 && arg_count == 1 &&
        effective_arg_types && effective_arg_types[0] &&
        (!return_type || XR_TYPE_IS_UNKNOWN(return_type))) {
        return_type = effective_arg_types[0];
    }

    if (xa_call_is_bytespan_load_le(call, callee_obj_type))
        return_type = xa_load_le_return_type(ctx, node, call, "ByteSpan.loadLE<T>()");

    if (xa_call_is_rawptr_load_le_unchecked(call, callee_obj_type))
        return_type = xa_load_le_return_type(ctx, node, call, "RawPtr.loadLEUnchecked<T>()");

    // Apply type substitution for generic method calls: obj.method<T>()
    if (callee_obj_type && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;

        // Look up method in class
        if (XR_TYPE_IS_INSTANCE(callee_obj_type) && callee_obj_type->instance.class_name) {
            XaSymbol *class_sym =
                xa_lookup_visible_class_symbol(ctx, callee_obj_type->instance.class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (class_links && class_links->class_info) {
                    XaSymbol *method_sym =
                        xa_class_info_lookup_instance_member(class_links->class_info, ma->name);
                    if (method_sym && method_sym->kind == XA_SYM_METHOD) {
                        XaSymbolLinks *method_links =
                            xa_analyzer_get_links(ctx->analyzer, method_sym);
                        if (method_links) {
                            // Apply method's own type parameters
                            return_type = xa_substitute_generic_call(ctx, method_links, callee_type,
                                                                     return_type, call, arg_count,
                                                                     effective_arg_types);

                            // Also apply class type parameters substitution
                            int class_type_param_count =
                                xa_symbol_links_get_type_param_count(class_links);
                            if (class_type_param_count > 0 &&
                                callee_obj_type->instance.type_arg_count > 0) {
                                const char **class_param_names =
                                    xr_malloc(sizeof(const char *) * class_type_param_count);
                                for (int i = 0; i < class_type_param_count; i++) {
                                    class_param_names[i] =
                                        xa_symbol_links_get_type_param_name(class_links, i);
                                }
                                return_type = xr_type_substitute(
                                    ctx->analyzer->isolate, return_type, class_param_names,
                                    callee_obj_type->instance.type_args,
                                    callee_obj_type->instance.type_arg_count);
                                xr_free(class_param_names);
                            }
                        }
                    }
                }
            }
        }
    }

    XrType *final_type = return_type ? return_type : xr_type_new_unknown(NULL);
    if (optional_function_call && final_type && !XR_TYPE_IS_UNKNOWN(final_type))
        final_type = xr_type_make_nullable(ctx->analyzer->isolate,
                                           xr_type_copy(ctx->analyzer->isolate, final_type));
    xr_free(effective_arg_types);
    xr_free(effective_arg_symbol_ids);
    xr_free(effective_arg_names);
    xr_free(effective_arg_modes);
    return final_type;
}
