/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor.c - AST visitor implementation
 *
 * KEY CONCEPT:
 *   Two-pass analysis: Pass 1 collects symbols (with hoisting for functions/classes),
 *   Pass 2 infers types and performs type checking.
 *
 */

#include "xanalyzer_visitor_internal.h"
#include "xaddressability.h"
#include "xanalyzer_ast_visitor.h"
#include "xanalyzer_errorset.h"
#include "xanalyzer_allocation.h"
#include "xanalyzer_suspend.h"
#include "xanalyzer_xrd.h"
#include "xconsteval.h"
#include "xtype_ref_resolve.h"
#include "../parser/xtype_ref.h"
#include "../../base/xchecks.h"
#include "../../base/xhashmap.h"
#include "../../base/xstorage.h"
#include "../../module/xmodule_graph.h"
#include "../../runtime/value/xstruct_layout.h"
#include "../../runtime/value/xtype_internal.h"
#include "../../shared/xr_derive_flags.h"
#include "../../toolchain/xcompiler_session.h"
#include <stdio.h>
#include <string.h>

void xa_visit_collect(XaInferContext *ctx, AstNode *node);
static XrClassInfo *member_set_class_info(XaInferContext *ctx, XrType *type,
                                          XaSymbolLinks **out_links);
static bool member_set_out_field_path_append(char *dst, size_t dst_size, const char *suffix);
static XaSymbol *member_set_out_field_path_symbol(XaInferContext *ctx, AstNode *expr, char *path,
                                                  size_t path_size);
static XrType *member_set_out_field_type_from_object(XaInferContext *ctx, XrType *obj_type,
                                                     const char *member_name);
static XrType *member_set_out_field_object_type_without_receiver_read(XaInferContext *ctx,
                                                                      AstNode *object,
                                                                      const char *target_path);

static const char *object_shape_type_label_local(XrType *type) {
    if (XR_TYPE_HAS_OBJECT_SHAPE(type) && type->object.type_name)
        return type->object.type_name;
    return xr_type_to_string(type);
}

static void xa_bind_param_default_exprs(XaInferContext *ctx, AstNode **defaults,
                                        XrType **param_types, int count) {
    if (!ctx || !defaults || count <= 0)
        return;
    XrType *saved_expected = ctx->expected_type;
    for (int i = 0; i < count; i++) {
        if (defaults[i]) {
            ctx->expected_type = param_types ? param_types[i] : NULL;
            xa_visit_infer_expr(ctx, defaults[i]);
        }
    }
    ctx->expected_type = saved_expected;
}

static void xa_reset_symbol_move_state_cb(const char *key, void *value, void *userdata) {
    (void) key;
    (void) userdata;
    XaSymbol *sym = (XaSymbol *) value;
    if (!sym)
        return;
    sym->links.move_state = XA_MOVE_NOT_MOVED;
    sym->links.moved_line = 0;
    sym->links.moved_column = 0;
}

static void xa_reset_scope_move_states(XaScope *scope) {
    if (!scope)
        return;
    xr_hashmap_foreach((XrHashMap *) scope->symbols, xa_reset_symbol_move_state_cb, NULL);
    for (int i = 0; i < scope->child_count; i++)
        xa_reset_scope_move_states(scope->children[i]);
}

XR_FUNC bool xa_expr_is_sys_thread_spawn_call(AstNode *expr) {
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
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

static void xa_warn_discarded_sys_thread_spawn(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !expr || !xa_expr_is_sys_thread_spawn_call(expr))
        return;
    const char *message =
        "sys.Thread.spawn returns a Thread handle; call join() or detach() explicitly";
    XrLocation loc = {.file = ctx->file_path, .line = expr->line, .column = expr->column};
    for (XaDiagnostic *d = ctx->analyzer ? ctx->analyzer->diagnostics : NULL; d; d = d->next) {
        if (d->severity == XR_DIAG_SEV_WARNING && d->code == XR_ERR_ANALYZE &&
            d->location.line == loc.line && d->location.column == loc.column &&
            ((d->location.file == loc.file) ||
             (d->location.file && loc.file && strcmp(d->location.file, loc.file) == 0)) &&
            d->message && strcmp(d->message, message) == 0) {
            return;
        }
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, message, &loc);
}

static AstNode *xa_discard_lint_unwrap_expr(AstNode *expr) {
    while (expr && (expr->type == AST_GROUPING || expr->type == AST_FORCE_UNWRAP))
        expr = expr->type == AST_GROUPING ? expr->as.grouping : expr->as.unary.operand;
    return expr;
}

static bool xa_process_options_expr_has_detached_literal(AstNode *expr) {
    expr = xa_discard_lint_unwrap_expr(expr);
    if (!expr)
        return false;
    AstNode **args = NULL;
    int arg_count = 0;
    if (expr->type == AST_CALL_EXPR) {
        args = expr->as.call_expr.arguments;
        arg_count = expr->as.call_expr.arg_count;
    } else if (expr->type == AST_NEW_EXPR) {
        args = expr->as.new_expr.arguments;
        arg_count = expr->as.new_expr.arg_count;
    }
    if (!args || arg_count < 6 || !args[5])
        return false;
    AstNode *detached = xa_discard_lint_unwrap_expr(args[5]);
    return detached && detached->type == AST_LITERAL_TRUE;
}

static bool xa_process_spawn_discard_is_detached(XaInferContext *ctx, CallExprNode *call) {
    if (!ctx || !ctx->analyzer || !call || call->arg_count < 3)
        return false;
    AstNode *options = xa_discard_lint_unwrap_expr(call->arguments[2]);
    if (!xa_process_options_expr_has_detached_literal(options))
        return false;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, options);
    const char *type_name = type ? xr_type_get_class_name(type) : NULL;
    return type_name && strcmp(type_name, "ProcessOptions") == 0;
}

static bool xa_expr_is_sys_os_resource_factory_call(AstNode *expr, const char **factory_name,
                                                    const char **type_name,
                                                    const char **close_method) {
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *method = &call->callee->as.member_access;
    if (!method->name || !method->object || method->object->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *owner = &method->object->as.member_access;
    if (!owner->name || !owner->object || owner->object->type != AST_VARIABLE)
        return false;
    const char *module_name = owner->object->as.variable.name;
    if (!module_name || strcmp(module_name, "sys") != 0)
        return false;
    if (strcmp(owner->name, "Process") == 0 && strcmp(method->name, "spawn") == 0) {
        if (factory_name)
            *factory_name = "sys.Process.spawn";
        if (type_name)
            *type_name = "Process";
        if (close_method)
            *close_method = "wait";
        return true;
    }
    if (strcmp(owner->name, "Pipe") == 0 && strcmp(method->name, "open") == 0) {
        if (factory_name)
            *factory_name = "sys.Pipe.open";
        if (type_name)
            *type_name = "Pipe";
        if (close_method)
            *close_method = "close";
        return true;
    }
    return false;
}

static void xa_warn_discarded_sys_os_resource_factory(XaInferContext *ctx, AstNode *expr) {
    const char *factory_name = NULL;
    const char *type_name = NULL;
    const char *close_method = NULL;
    if (!ctx || !expr ||
        !xa_expr_is_sys_os_resource_factory_call(expr, &factory_name, &type_name, &close_method))
        return;
    if (factory_name && strcmp(factory_name, "sys.Process.spawn") == 0 &&
        expr->type == AST_CALL_EXPR &&
        xa_process_spawn_discard_is_detached(ctx, &expr->as.call_expr))
        return;

    char message[192];
    snprintf(message, sizeof(message), "%s returns a %s handle; call %s() explicitly", factory_name,
             type_name, close_method);
    XrLocation loc = {.file = ctx->file_path, .line = expr->line, .column = expr->column};
    for (XaDiagnostic *d = ctx->analyzer ? ctx->analyzer->diagnostics : NULL; d; d = d->next) {
        if (d->severity == XR_DIAG_SEV_WARNING && d->code == XR_ERR_ANALYZE &&
            d->location.line == loc.line && d->location.column == loc.column &&
            ((d->location.file == loc.file) ||
             (d->location.file && loc.file && strcmp(d->location.file, loc.file) == 0)) &&
            d->message && strcmp(d->message, message) == 0) {
            return;
        }
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, message, &loc);
}

static XrType *xa_view_source_element_type(XrType *type) {
    if (!type)
        return NULL;
    if ((XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_VIEW(type) || XR_TYPE_IS_SPAN(type)) &&
        type->container.element_type)
        return type->container.element_type;
    if (type->kind == XR_KIND_FIXED_ARRAY && type->fixed_array.element_type)
        return type->fixed_array.element_type;
    return NULL;
}

static bool xa_type_is_span_slice_source(XrType *type) {
    return type &&
           (XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_SPAN(type) || type->kind == XR_KIND_FIXED_ARRAY);
}

static bool xa_type_is_byte_array_or_span(XrType *type) {
    if (!xa_type_is_span_slice_source(type))
        return false;
    XrType *elem = xa_view_source_element_type(type);
    return xr_type_is_exact_u8(elem);
}

static bool xa_type_is_byte_array_or_view(XrType *type) {
    if (!type || (!XR_TYPE_IS_ARRAY(type) && !XR_TYPE_IS_VIEW(type)))
        return false;
    XrType *elem = type->container.element_type;
    return xr_type_is_exact_u8(elem);
}

static bool xa_enum_payload_contains_by_value(XrType *type, const char *enum_name) {
    if (!type || !enum_name)
        return false;

    switch (type->kind) {
        case XR_KIND_ENUM:
            return type->enum_type.enum_name && strcmp(type->enum_type.enum_name, enum_name) == 0;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            return type->instance.class_name && strcmp(type->instance.class_name, enum_name) == 0;
        case XR_KIND_FIXED_ARRAY:
            return xa_enum_payload_contains_by_value(type->fixed_array.element_type, enum_name);
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++) {
                if (xa_enum_payload_contains_by_value(type->tuple.element_types[i], enum_name))
                    return true;
            }
            return false;
        case XR_KIND_UNION:
            for (int i = 0; i < type->union_type.member_count; i++) {
                if (xa_enum_payload_contains_by_value(type->union_type.members[i], enum_name))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static XrType *xa_span_type_from_view_source(XaInferContext *ctx, XrType *src) {
    if (!ctx || !ctx->analyzer || !src)
        return xr_type_new_unknown(NULL);
    if (!xa_type_is_span_slice_source(src))
        return xr_type_new_unknown(NULL);
    XrType *elem = xa_view_source_element_type(src);
    if (!elem)
        elem = xr_type_new_unknown(NULL);
    if (xa_type_is_byte_array_or_span(src))
        return xr_type_new_u8_slice(ctx->analyzer->isolate);
    return xr_type_new_span(ctx->analyzer->isolate, elem);
}

static XrType *xa_storage_view_type_from_source(XaInferContext *ctx, XrType *src) {
    if (!ctx || !ctx->analyzer || !src)
        return xr_type_new_unknown(NULL);
    if (!XR_TYPE_IS_ARRAY(src) && !XR_TYPE_IS_VIEW(src))
        return xr_type_new_unknown(NULL);
    XrType *elem =
        src->container.element_type ? src->container.element_type : xr_type_new_unknown(NULL);
    if (xa_type_is_byte_array_or_view(src))
        return xr_type_new_u8_view(ctx->analyzer->isolate);
    return xr_type_new_view(ctx->analyzer->isolate, elem);
}

XR_FUNC void xa_report_view_expr_requires_target(XaInferContext *ctx, AstNode *node,
                                                 const char *kind) {
    if (!ctx || !ctx->analyzer || !node)
        return;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[360];
    snprintf(msg, sizeof(msg),
             "%s result requires an explicit target type; use ': Slice<T>' for a scoped borrowed "
             "view, or copy(...) for owned data",
             kind ? kind : "view expression");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE, msg,
                               &loc);
}

static XrType *xa_report_invalid_slice_source(XaInferContext *ctx, AstNode *node, XrType *src) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_error(NULL);
    if (src && XR_TYPE_IS_ERROR(src))
        return xr_type_new_error(ctx->analyzer->isolate);
    if (!src || src->kind == XR_KIND_UNKNOWN)
        return xr_type_new_unknown(ctx->analyzer->isolate);

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[192];
    snprintf(msg, sizeof(msg), "type '%s' does not support slice syntax", xr_type_to_string(src));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
    return xr_type_new_error(ctx->analyzer->isolate);
}

static bool xa_span_target_matches_source(XrType *target, XrType *src) {
    if (!target || !src || !XR_TYPE_IS_SPAN(target))
        return false;
    if (!xa_type_is_span_slice_source(src))
        return false;
    XrType *target_elem = target->container.element_type;
    XrType *src_elem = xa_view_source_element_type(src);
    if (!target_elem || !src_elem)
        return true;
    if (XR_TYPE_IS_UNKNOWN(target_elem) || XR_TYPE_IS_UNKNOWN(src_elem))
        return true;
    return xr_type_assignable(target_elem, src_elem) && xr_type_assignable(src_elem, target_elem);
}

static bool xa_view_target_matches_source(XrType *target, XrType *src) {
    if (!target || !src || !XR_TYPE_IS_VIEW(target))
        return false;
    if (!XR_TYPE_IS_ARRAY(src) && !XR_TYPE_IS_VIEW(src))
        return false;
    XrType *target_elem = target->container.element_type;
    XrType *src_elem = src->container.element_type;
    if (!target_elem || !src_elem)
        return true;
    if (XR_TYPE_IS_UNKNOWN(target_elem) || XR_TYPE_IS_UNKNOWN(src_elem))
        return true;
    return xr_type_assignable(target_elem, src_elem) && xr_type_assignable(src_elem, target_elem);
}

XR_FUNC const char *xa_concurrency_handle_label(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_CHANNEL)
        return "Channel";
    if (xr_type_is_named_class(type, "Atomic"))
        return "Atomic";
    if (xr_type_is_named_class(type, "WorkQueue"))
        return "WorkQueue";
    if (xr_type_is_named_class(type, "ResultGroup"))
        return "ResultGroup";
    if (xr_type_is_named_class(type, "CountdownLatch"))
        return "CountdownLatch";
    if (xr_type_is_named_class(type, "Semaphore"))
        return "Semaphore";
    if (xr_type_is_named_class(type, "EventCount"))
        return "EventCount";
    if (xr_type_is_named_class(type, "OsMutex"))
        return "OsMutex";
    if (xr_type_is_named_class(type, "OsRwLock"))
        return "OsRwLock";
    if (xr_type_is_named_class(type, "OsCondvar"))
        return "OsCondvar";
    if (xr_type_is_named_class(type, "OsBarrier"))
        return "OsBarrier";
    if (xr_type_is_named_class(type, "OsOnce"))
        return "OsOnce";
    return NULL;
}

XR_FUNC bool xa_type_is_concurrency_handle(const XrType *type) {
    return xa_concurrency_handle_label(type) != NULL;
}

XR_FUNC bool xa_freestanding_profile_enabled(XaAnalyzer *analyzer) {
    return xa_analyzer_is_freestanding(analyzer);
}

XR_FUNC bool xa_freestanding_stdlib_module_known(const char *module_name) {
    static const char *modules[] = {"prelude",  "time", "math",     "path",   "base64", "regex",
                                    "mem",      "sync", "parallel", "simd",   "sys",    "url",
                                    "datetime", "log",  "encoding", "_probe", "io",     "os",
                                    "json",     "net",  "http",     "crypto", "csv",    "toml",
                                    "yaml",     "xml",  "compress", "ws"};
    if (!module_name)
        return false;
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        if (strcmp(module_name, modules[i]) == 0)
            return true;
    }
    return false;
}

XR_FUNC bool xa_freestanding_stdlib_module_allowed(const char *module_name) {
    if (!module_name)
        return false;
    return strcmp(module_name, "prelude") == 0 || strcmp(module_name, "math") == 0 ||
           strcmp(module_name, "mem") == 0;
}

static bool xa_freestanding_math_member_allowed(const char *member_name) {
    if (!member_name)
        return true;
    return strcmp(member_name, "PI") == 0 || strcmp(member_name, "E") == 0 ||
           strcmp(member_name, "TAU") == 0 || strcmp(member_name, "SQRT2") == 0 ||
           strcmp(member_name, "LN2") == 0 || strcmp(member_name, "LN10") == 0 ||
           strcmp(member_name, "LOG2E") == 0 || strcmp(member_name, "LOG10E") == 0 ||
           strcmp(member_name, "EPSILON") == 0 || strcmp(member_name, "MAX_INT") == 0 ||
           strcmp(member_name, "MIN_INT") == 0 || strcmp(member_name, "MAX_FLOAT") == 0 ||
           strcmp(member_name, "INF") == 0 || strcmp(member_name, "NAN") == 0 ||
           strcmp(member_name, "min") == 0 || strcmp(member_name, "max") == 0 ||
           strcmp(member_name, "clamp") == 0;
}

XR_FUNC bool xa_freestanding_stdlib_member_allowed(const char *module_name,
                                                   const char *member_name) {
    if (!module_name || !member_name)
        return true;
    if (xa_freestanding_stdlib_module_known(module_name) &&
        !xa_freestanding_stdlib_module_allowed(module_name))
        return false;
    if (strcmp(module_name, "math") == 0)
        return xa_freestanding_math_member_allowed(member_name);
    return true;
}

XR_FUNC const char *xa_freestanding_stdlib_member_reject_suggestion(const char *module_name) {
    if (module_name && strcmp(module_name, "parallel") == 0) {
        return "parallel uses the hosted CPU batch executor; freestanding code must use explicit "
               "raw loops or a platform-specific runtime";
    }
    if (module_name && strcmp(module_name, "math") == 0) {
        return "libm-backed and system-random math helpers are not part of the freestanding "
               "no-libc subset yet";
    }
    return "this stdlib member is not part of the freestanding allowlist yet";
}

XR_FUNC void xa_freestanding_report_unavailable(XaInferContext *ctx, AstNode *node,
                                                const char *feature, const char *suggestion) {
    if (!ctx || !ctx->analyzer || !node || !xa_freestanding_profile_enabled(ctx->analyzer))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[384];
    if (suggestion && suggestion[0]) {
        snprintf(msg, sizeof(msg), "freestanding profile rejects %s; %s",
                 feature ? feature : "this construct", suggestion);
    } else {
        snprintf(msg, sizeof(msg), "freestanding profile rejects %s",
                 feature ? feature : "this construct");
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg, &loc);
}

XR_FUNC void xa_report_unknown_stdlib_member(XaInferContext *ctx, AstNode *node,
                                             const char *module_name, const char *member_name) {
    if (!ctx || !ctx->analyzer || !node || !module_name || !member_name)
        return;
    if (!xa_builtin_get_module_info(module_name)) {
        const char *xrd_error = xa_xrd_last_error();
        if (xrd_error && xrd_error[0]) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[384];
            snprintf(msg, sizeof(msg), "invalid XRD descriptor for module '%s': %s", module_name,
                     xrd_error);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg, &loc);
        }
        return;
    }

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    snprintf(msg, sizeof(msg), "stdlib module '%s' has no member '%s'", module_name, member_name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg, &loc);
}

XR_FUNC void xa_parallel_capture_check(XaInferContext *ctx, AstNode *loc_node, XaSymbol *sym,
                                       bool is_write) {
    if (!ctx || !ctx->in_parallel_callback_body || !ctx->parallel_callback_scope || !loc_node ||
        !sym)
        return;
    if (sym->scope == ctx->parallel_callback_scope ||
        xa_scope_is_descendant(sym->scope, ctx->parallel_callback_scope)) {
        return;
    }
    if (sym->kind != XA_SYM_VARIABLE && sym->kind != XA_SYM_PARAMETER)
        return;

    const char *name = sym->name ? sym->name : "?";
    const char *callback_name =
        ctx->parallel_callback_name ? ctx->parallel_callback_name : "parallel callback";
    XrLocation loc = {.file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
    char msg[256];
    XrType *sym_type = xa_analyzer_get_type(ctx->analyzer, sym);

    if (is_write) {
        snprintf(msg, sizeof(msg),
                 "%s cannot assign to captured variable '%s'; use Atomic<T>, parallel.reduce, "
                 "or Plan state",
                 callback_name, name);
    } else if (sym->is_shared) {
        return;
    } else if (xa_type_is_concurrency_handle(sym_type)) {
        return;
    } else if (sym->kind == XA_SYM_PARAMETER) {
        snprintf(msg, sizeof(msg),
                 "%s cannot capture parameter '%s'; copy immutable data to a const or shared "
                 "binding",
                 callback_name, name);
    } else if (sym->is_const) {
        return;
    } else {
        snprintf(msg, sizeof(msg),
                 "%s cannot capture mutable variable '%s'; copy it to a const, use Atomic<T>, "
                 "or use Plan state",
                 callback_name, name);
    }

    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CLOSURE_CAPTURE,
                               msg, &loc);
}

typedef struct XaParallelCallbackEffectScan {
    XaInferContext *ctx;
    const char *callback_name;
    XaSymbol *call_stack[32];
    int call_depth;
    int nested_function_depth;
    bool report;
    bool found;
    bool found_suspend;
    bool reported;
    char feature_buf[192];
} XaParallelCallbackEffectScan;

typedef enum XaParallelFunctionValueStatus {
    XA_PARALLEL_FN_VALUE_SAFE = 0,
    XA_PARALLEL_FN_VALUE_EFFECT,
    XA_PARALLEL_FN_VALUE_DYNAMIC,
} XaParallelFunctionValueStatus;

static bool xa_parallel_assert_call_name(const char *name) {
    return name && (strcmp(name, "assert") == 0 || strcmp(name, "assert_true") == 0 ||
                    strcmp(name, "assert_false") == 0 || strcmp(name, "assert_eq") == 0 ||
                    strcmp(name, "assert_ne") == 0 || strcmp(name, "assert_throws") == 0);
}

static XaScope *xa_parallel_find_function_scope_for_symbol(XaScope *scope, XaSymbol *sym) {
    if (!scope || !sym)
        return NULL;
    if (scope->function_symbol == sym)
        return scope;
    for (int i = 0; i < scope->child_count; i++) {
        XaScope *found = xa_parallel_find_function_scope_for_symbol(scope->children[i], sym);
        if (found)
            return found;
    }
    return NULL;
}

static AstNode *xa_parallel_symbol_function_body(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !ctx->analyzer || !sym || sym->kind != XA_SYM_FUNCTION)
        return NULL;
    XaScope *scope = xa_parallel_find_function_scope_for_symbol(ctx->analyzer->global_scope, sym);
    AstNode *fn_node = scope ? (AstNode *) scope->ast_node : NULL;
    if (!fn_node)
        return NULL;
    if (fn_node->type == AST_FUNCTION_DECL)
        return fn_node->as.function_decl.body;
    if (fn_node->type == AST_FUNCTION_EXPR)
        return fn_node->as.function_expr.body;
    return NULL;
}

static XaSymbol *xa_parallel_symbol_from_variable_node(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || node->type != AST_VARIABLE)
        return NULL;
    uint32_t symbol_id = node->as.variable.symbol_id;
    if (symbol_id != 0) {
        XaSymbol *sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, symbol_id);
        if (sym)
            return sym;
    }
    const char *name = node->as.variable.name;
    return name ? xa_lookup_visible_symbol(ctx, name) : NULL;
}

static XaSymbol *xa_parallel_import_target_symbol(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !sym || !sym->is_imported)
        return sym;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || !links->module_name)
        return sym;

    bool is_quoted = links->module_name[0] == '.' || links->module_name[0] == '/';
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, links->module_name, is_quoted);
    if (!exports)
        return sym;
    const char *member_name = links->import_member_name ? links->import_member_name : sym->name;
    XaSymbol *target = member_name ? (XaSymbol *) xr_hashmap_get(exports, member_name) : NULL;
    return target ? target : sym;
}

static XaSymbol *xa_parallel_module_member_symbol(XaInferContext *ctx, AstNode *callee) {
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object || ma->object->type != AST_VARIABLE ||
        !ma->object->as.variable.name)
        return NULL;

    XaSymbol *mod_sym = xa_parallel_symbol_from_variable_node(ctx, ma->object);
    if (!mod_sym || mod_sym->kind != XA_SYM_MODULE)
        return NULL;

    XaSymbolLinks *mod_links = xa_analyzer_get_links(ctx->analyzer, mod_sym);
    const char *mod_name =
        mod_links && mod_links->module_name ? mod_links->module_name : ma->object->as.variable.name;
    bool is_quoted = mod_name[0] == '.' || mod_name[0] == '/';
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, mod_name, is_quoted);
    if (!exports)
        return NULL;
    XaSymbol *member = (XaSymbol *) xr_hashmap_get(exports, ma->name);
    return xa_parallel_import_target_symbol(ctx, member);
}

static bool xa_out_da_stmt_may_throw(XaInferContext *ctx, AstNode *node, int depth);
static bool xa_out_da_expr_may_throw(XaInferContext *ctx, AstNode *node, int depth);

static bool xa_out_da_expr_list_may_throw(XaInferContext *ctx, AstNode **nodes, int count,
                                          int depth) {
    for (int i = 0; i < count; i++) {
        if (xa_out_da_expr_may_throw(ctx, nodes ? nodes[i] : NULL, depth))
            return true;
    }
    return false;
}

static AstNode *xa_out_da_symbol_function_body(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !ctx->analyzer || !sym || sym->kind != XA_SYM_FUNCTION)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    AstNode *fn_node = links ? links->function_decl_node : NULL;
    if (!fn_node)
        return xa_parallel_symbol_function_body(ctx, sym);
    if (fn_node->type == AST_FUNCTION_DECL)
        return fn_node->as.function_decl.body;
    if (fn_node->type == AST_FUNCTION_EXPR)
        return fn_node->as.function_expr.body;
    return NULL;
}

static bool xa_out_da_symbol_may_throw(XaInferContext *ctx, XaSymbol *sym, int depth) {
    if (!ctx || !ctx->analyzer || !sym || depth > 8)
        return true;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || sym->kind != XA_SYM_FUNCTION || sym->is_imported || links->is_extern)
        return true;
    if (links->effect_id != XA_EFFECT_NONE) {
        const XaEffectSummary *summary =
            xa_effect_db_get(ctx->analyzer->effect_db, links->effect_id);
        return !summary || !xa_effect_summary_is_nothrow(summary);
    }
    AstNode *body = xa_out_da_symbol_function_body(ctx, sym);
    if (!body)
        return true;
    return xa_out_da_stmt_may_throw(ctx, body, depth + 1);
}

static bool xa_out_da_call_may_throw(XaInferContext *ctx, AstNode *callee, int depth) {
    if (!callee || depth > 8)
        return true;
    if (callee->type == AST_FUNCTION_DECL)
        return xa_out_da_stmt_may_throw(ctx, callee->as.function_decl.body, depth + 1);
    if (callee->type == AST_FUNCTION_EXPR)
        return xa_out_da_stmt_may_throw(ctx, callee->as.function_expr.body, depth + 1);
    if (callee->type != AST_VARIABLE)
        return true;
    return xa_out_da_symbol_may_throw(ctx, xa_parallel_symbol_from_variable_node(ctx, callee),
                                      depth + 1);
}

static bool xa_out_da_match_arms_may_throw(XaInferContext *ctx, MatchExprNode *match, int depth) {
    if (!match)
        return false;
    for (int i = 0; i < match->arm_count; i++) {
        AstNode *arm = match->arms ? match->arms[i] : NULL;
        if (!arm || arm->type != AST_MATCH_ARM)
            return true;
        if (xa_out_da_expr_may_throw(ctx, arm->as.match_arm.guard, depth) ||
            xa_out_da_stmt_may_throw(ctx, arm->as.match_arm.body, depth))
            return true;
    }
    return false;
}

static bool xa_out_da_expr_may_throw(XaInferContext *ctx, AstNode *node, int depth) {
    if (!node)
        return false;
    if (depth > 8)
        return true;

    switch (node->type) {
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_FIXED_BYTES_LITERAL:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_VARIABLE:
        case AST_THIS_EXPR:
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return false;
        case AST_CALL_EXPR: {
            CallExprNode *call = &node->as.call_expr;
            return xa_out_da_expr_may_throw(ctx, call->callee, depth + 1) ||
                   xa_out_da_expr_list_may_throw(ctx, call->arguments, call->arg_count,
                                                 depth + 1) ||
                   xa_out_da_call_may_throw(ctx, call->callee, depth + 1);
        }
        case AST_GROUPING:
            return xa_out_da_expr_may_throw(ctx, node->as.grouping, depth + 1);
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
        case AST_MOVE_EXPR:
        case AST_COMPTIME_EXPR:
        case AST_UNSAFE_EXPR:
            return xa_out_da_expr_may_throw(ctx, node->as.unary.operand, depth + 1);
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
        case AST_NULLISH_COALESCE:
            return xa_out_da_expr_may_throw(ctx, node->as.binary.left, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.binary.right, depth + 1);
        case AST_TERNARY:
            return xa_out_da_expr_may_throw(ctx, node->as.ternary.condition, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.ternary.true_expr, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.ternary.false_expr, depth + 1);
        case AST_ASSIGNMENT:
            return xa_out_da_expr_may_throw(ctx, node->as.assignment.value, depth + 1);
        case AST_COMPOUND_ASSIGNMENT:
            return xa_out_da_expr_may_throw(ctx, node->as.compound_assignment.object, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.compound_assignment.value, depth + 1);
        case AST_DESTRUCTURE_ASSIGN:
            return xa_out_da_expr_may_throw(ctx, node->as.destructure_assign.value, depth + 1);
        case AST_MEMBER_ACCESS:
            return xa_out_da_expr_may_throw(ctx, node->as.member_access.object, depth + 1);
        case AST_MEMBER_SET:
            return xa_out_da_expr_may_throw(ctx, node->as.member_set.object, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.member_set.value, depth + 1);
        case AST_INDEX_GET:
            return xa_out_da_expr_may_throw(ctx, node->as.index_get.array, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.index_get.index, depth + 1);
        case AST_INDEX_SET:
            return xa_out_da_expr_may_throw(ctx, node->as.index_set.array, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.index_set.index, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.index_set.value, depth + 1);
        case AST_SLICE_EXPR:
            return xa_out_da_expr_may_throw(ctx, node->as.slice_expr.source, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.slice_expr.start, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.slice_expr.end, depth + 1);
        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat)
                return xa_out_da_expr_may_throw(ctx, node->as.array_literal.repeat_value,
                                                depth + 1) ||
                       xa_out_da_expr_may_throw(ctx, node->as.array_literal.repeat_count,
                                                depth + 1);
            return xa_out_da_expr_list_may_throw(ctx, node->as.array_literal.elements,
                                                 node->as.array_literal.count, depth + 1);
        case AST_TUPLE_LITERAL:
            return xa_out_da_expr_list_may_throw(ctx, node->as.tuple_literal.elements,
                                                 node->as.tuple_literal.count, depth + 1);
        case AST_OBJECT_LITERAL:
            return xa_out_da_expr_list_may_throw(ctx, node->as.object_literal.keys,
                                                 node->as.object_literal.count, depth + 1) ||
                   xa_out_da_expr_list_may_throw(ctx, node->as.object_literal.values,
                                                 node->as.object_literal.count, depth + 1);
        case AST_MAP_LITERAL:
            return xa_out_da_expr_list_may_throw(ctx, node->as.map_literal.keys,
                                                 node->as.map_literal.count, depth + 1) ||
                   xa_out_da_expr_list_may_throw(ctx, node->as.map_literal.values,
                                                 node->as.map_literal.count, depth + 1);
        case AST_SET_LITERAL:
            return xa_out_da_expr_list_may_throw(ctx, node->as.set_literal.elements,
                                                 node->as.set_literal.count, depth + 1);
        case AST_STRUCT_LITERAL:
            return xa_out_da_expr_list_may_throw(ctx, node->as.struct_literal.field_values,
                                                 node->as.struct_literal.field_count, depth + 1);
        case AST_TEMPLATE_STRING:
            return xa_out_da_expr_list_may_throw(ctx, node->as.template_str.parts,
                                                 node->as.template_str.part_count, depth + 1);
        case AST_MATCH_EXPR:
            return xa_out_da_expr_may_throw(ctx, node->as.match_expr.expr, depth + 1) ||
                   xa_out_da_match_arms_may_throw(ctx, &node->as.match_expr, depth + 1);
        case AST_SCOPE_BLOCK:
            return xa_out_da_stmt_may_throw(ctx, node->as.scope_block.body, depth + 1);
        case AST_AS_EXPR:
            return xa_out_da_expr_may_throw(ctx, node->as.as_expr.expr, depth + 1);
        case AST_IS_EXPR:
            return xa_out_da_expr_may_throw(ctx, node->as.is_expr.expr, depth + 1);
        case AST_AWAIT_EXPR:
        case AST_GO_EXPR:
        case AST_NEW_EXPR:
            return true;
        default:
            return true;
    }
}

static bool xa_out_da_stmt_may_throw(XaInferContext *ctx, AstNode *node, int depth) {
    if (!node)
        return false;
    if (depth > 8)
        return true;

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                if (xa_out_da_stmt_may_throw(ctx, node->as.block.statements[i], depth + 1))
                    return true;
            }
            return false;
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                if (xa_out_da_stmt_may_throw(ctx, node->as.program.statements[i], depth + 1))
                    return true;
            }
            return false;
        case AST_EXPR_STMT:
            return xa_out_da_expr_may_throw(ctx, node->as.expr_stmt, depth + 1);
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            return xa_out_da_expr_may_throw(ctx, node->as.var_decl.initializer, depth + 1);
        case AST_DESTRUCTURE_DECL:
            return xa_out_da_expr_may_throw(ctx, node->as.destructure_decl.initializer, depth + 1);
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
        case AST_DESTRUCTURE_ASSIGN:
        case AST_MEMBER_SET:
        case AST_INDEX_SET:
        case AST_MATCH_EXPR:
            return xa_out_da_expr_may_throw(ctx, node, depth + 1);
        case AST_RETURN_STMT:
            return xa_out_da_expr_list_may_throw(ctx, node->as.return_stmt.values,
                                                 node->as.return_stmt.value_count, depth + 1);
        case AST_THROW_STMT:
            return true;
        case AST_IF_STMT:
            return xa_out_da_expr_may_throw(ctx, node->as.if_stmt.condition, depth + 1) ||
                   xa_out_da_stmt_may_throw(ctx, node->as.if_stmt.then_branch, depth + 1) ||
                   xa_out_da_stmt_may_throw(ctx, node->as.if_stmt.else_branch, depth + 1);
        case AST_WHILE_STMT:
            return xa_out_da_expr_may_throw(ctx, node->as.while_stmt.condition, depth + 1) ||
                   xa_out_da_stmt_may_throw(ctx, node->as.while_stmt.body, depth + 1);
        case AST_FOR_STMT:
            return xa_out_da_stmt_may_throw(ctx, node->as.for_stmt.initializer, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.for_stmt.condition, depth + 1) ||
                   xa_out_da_expr_may_throw(ctx, node->as.for_stmt.increment, depth + 1) ||
                   xa_out_da_stmt_may_throw(ctx, node->as.for_stmt.body, depth + 1);
        case AST_FOR_IN_STMT:
            return xa_out_da_expr_may_throw(ctx, node->as.for_in_stmt.collection, depth + 1) ||
                   xa_out_da_stmt_may_throw(ctx, node->as.for_in_stmt.body, depth + 1);
        case AST_TRY_CATCH:
            if (xa_out_da_stmt_may_throw(ctx, node->as.try_catch.try_body, depth + 1))
                return true;
            for (int i = 0; i < node->as.try_catch.catch_count; i++) {
                XrCatchClause *cc =
                    node->as.try_catch.catch_clauses ? node->as.try_catch.catch_clauses[i] : NULL;
                if (cc && cc->is_panic)
                    return true;
                if (xa_out_da_stmt_may_throw(ctx, cc ? cc->body : NULL, depth + 1))
                    return true;
            }
            return false;
        case AST_PRINT_STMT:
            return xa_out_da_expr_list_may_throw(ctx, node->as.print_stmt.exprs,
                                                 node->as.print_stmt.expr_count, depth + 1);
        case AST_DEFER_STMT:
            return xa_out_da_expr_may_throw(ctx, node->as.defer_stmt.expr, depth + 1);
        case AST_SCOPE_BLOCK:
            return xa_out_da_stmt_may_throw(ctx, node->as.scope_block.body, depth + 1);
        default:
            return true;
    }
}

static bool xa_out_da_try_catch_may_enter_catch(XaInferContext *ctx, TryCatchNode *tc) {
    if (!tc)
        return true;
    for (int ci = 0; ci < tc->catch_count; ci++) {
        XrCatchClause *cc = tc->catch_clauses ? tc->catch_clauses[ci] : NULL;
        if (!cc || cc->is_panic)
            return true;
    }
    return xa_out_da_stmt_may_throw(ctx, tc->try_body, 0);
}

static bool xa_parallel_call_is_coro_yield(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || strcmp(ma->name, "yield") != 0 || !ma->object ||
        ma->object->type != AST_VARIABLE)
        return false;
    const char *module_name = ma->object->as.variable.name;
    if (!module_name || strcmp(module_name, "Coro") != 0)
        return false;
    XaSymbol *sym = ctx ? xa_parallel_symbol_from_variable_node(ctx, ma->object) : NULL;
    return sym && sym->kind == XA_SYM_MODULE && sym->is_builtin;
}

static XrType *xa_parallel_node_type(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return NULL;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, node);
    if (!type)
        type = xa_visit_infer_expr(ctx, node);
    return type;
}

static const char *xa_parallel_channel_blocking_method_feature(const char *name) {
    if (!name)
        return NULL;
    if (strcmp(name, "send") == 0)
        return "channel send";
    if (strcmp(name, "recv") == 0)
        return "channel recv";
    if (strcmp(name, "recvOr") == 0)
        return "channel recvOr";
    if (strcmp(name, "sendTimeout") == 0)
        return "channel sendTimeout";
    if (strcmp(name, "recvTimeout") == 0)
        return "channel recvTimeout";
    return NULL;
}

static const char *xa_parallel_channel_blocking_call_feature(XaInferContext *ctx,
                                                             CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    const char *feature = xa_parallel_channel_blocking_method_feature(ma->name);
    if (!feature || !ma->object)
        return NULL;
    XrType *receiver_type = xa_parallel_node_type(ctx, ma->object);
    return receiver_type && receiver_type->kind == XR_KIND_CHANNEL ? feature : NULL;
}

static const char *xa_parallel_stdlib_yieldable_feature(const char *module_name,
                                                        const char *member_name, char *feature_buf,
                                                        size_t feature_buf_size) {
    if (!module_name || !member_name || !feature_buf || feature_buf_size == 0 ||
        !xa_builtin_module_func_is_yieldable(module_name, member_name))
        return NULL;
    snprintf(feature_buf, feature_buf_size, "%s.%s()", module_name, member_name);
    return feature_buf;
}

static bool xa_parallel_builtin_module_func_exists(const char *module_name,
                                                   const char *member_name) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info(module_name);
    if (!mod || !member_name)
        return false;
    for (int i = 0; i < mod->function_count; i++) {
        const XaBuiltinMember *member = &mod->functions[i];
        if (member->is_method && member->name && strcmp(member->name, member_name) == 0)
            return true;
    }
    return false;
}

static bool xa_parallel_module_has_effect_summary(const char *module_name) {
    return xa_freestanding_stdlib_module_known(module_name) ||
           (module_name &&
            (strcmp(module_name, "Coro") == 0 || strcmp(module_name, "CoroPool") == 0));
}

static const char *xa_parallel_unknown_module_func_feature(const char *module_name,
                                                           const char *member_name,
                                                           char *feature_buf,
                                                           size_t feature_buf_size) {
    if (!module_name || !member_name || !feature_buf || feature_buf_size == 0 ||
        xa_parallel_module_has_effect_summary(module_name) ||
        !xa_parallel_builtin_module_func_exists(module_name, member_name))
        return NULL;
    snprintf(feature_buf, feature_buf_size, "call to unknown-effect function '%s.%s'", module_name,
             member_name);
    return feature_buf;
}

static const char *xa_parallel_imported_yieldable_feature(XaInferContext *ctx, XaSymbol *sym,
                                                          char *feature_buf,
                                                          size_t feature_buf_size) {
    if (!ctx || !ctx->analyzer || !sym || !sym->is_imported)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || !links->module_name)
        return NULL;
    const char *member_name = links->import_member_name ? links->import_member_name : sym->name;
    return xa_parallel_stdlib_yieldable_feature(links->module_name, member_name, feature_buf,
                                                feature_buf_size);
}

static const char *xa_parallel_imported_unknown_effect_feature(XaInferContext *ctx, XaSymbol *sym,
                                                               char *feature_buf,
                                                               size_t feature_buf_size) {
    if (!ctx || !ctx->analyzer || !sym || !sym->is_imported)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || !links->module_name)
        return NULL;
    const char *member_name = links->import_member_name ? links->import_member_name : sym->name;
    return xa_parallel_unknown_module_func_feature(links->module_name, member_name, feature_buf,
                                                   feature_buf_size);
}

static const char *xa_parallel_module_member_yieldable_feature(XaInferContext *ctx, AstNode *callee,
                                                               char *feature_buf,
                                                               size_t feature_buf_size) {
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object || ma->object->type != AST_VARIABLE)
        return NULL;
    XaSymbol *mod_sym = xa_parallel_symbol_from_variable_node(ctx, ma->object);
    if (!mod_sym || mod_sym->kind != XA_SYM_MODULE)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, mod_sym);
    const char *module_name =
        links && links->module_name ? links->module_name : ma->object->as.variable.name;
    return xa_parallel_stdlib_yieldable_feature(module_name, ma->name, feature_buf,
                                                feature_buf_size);
}

static const char *xa_parallel_module_member_unknown_effect_feature(XaInferContext *ctx,
                                                                    AstNode *callee,
                                                                    char *feature_buf,
                                                                    size_t feature_buf_size) {
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object || ma->object->type != AST_VARIABLE)
        return NULL;
    XaSymbol *mod_sym = xa_parallel_symbol_from_variable_node(ctx, ma->object);
    if (!mod_sym || mod_sym->kind != XA_SYM_MODULE)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, mod_sym);
    const char *module_name =
        links && links->module_name ? links->module_name : ma->object->as.variable.name;
    return xa_parallel_unknown_module_func_feature(module_name, ma->name, feature_buf,
                                                   feature_buf_size);
}

static bool xa_parallel_handle_method_exists(const XaBuiltinHandle *handle,
                                             const char *method_name) {
    if (!handle || !method_name)
        return false;
    for (int i = 0; i < handle->method_count; i++) {
        const XaBuiltinMember *method = &handle->methods[i];
        if (method->is_method && method->name && strcmp(method->name, method_name) == 0)
            return true;
    }
    return false;
}

static const char *xa_parallel_handle_method_unknown_effect_feature(XaInferContext *ctx,
                                                                    AstNode *callee,
                                                                    char *feature_buf,
                                                                    size_t feature_buf_size) {
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS || !feature_buf ||
        feature_buf_size == 0)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object)
        return NULL;
    XrType *receiver_type = xa_parallel_node_type(ctx, ma->object);
    const char *handle_name = receiver_type && XR_TYPE_IS_INSTANCE(receiver_type)
                                  ? receiver_type->instance.class_name
                                  : NULL;
    if (!handle_name)
        return NULL;
    const XaBuiltinHandle *handle = xa_builtin_find_handle_by_name(handle_name);
    if (!xa_parallel_handle_method_exists(handle, ma->name))
        return NULL;
    const char *module_name = xa_builtin_find_handle_module(handle_name);
    if (module_name && xa_parallel_module_has_effect_summary(module_name))
        return NULL;
    snprintf(feature_buf, feature_buf_size, "call to unknown-effect method '%s.%s'", handle_name,
             ma->name);
    return feature_buf;
}

static const char *xa_parallel_stdlib_suspend_call_feature(XaParallelCallbackEffectScan *scan,
                                                           CallExprNode *call) {
    if (!scan || !call || !call->callee)
        return NULL;
    XaInferContext *ctx = scan->ctx;
    const char *feature = xa_parallel_module_member_yieldable_feature(
        ctx, call->callee, scan->feature_buf, sizeof(scan->feature_buf));
    if (feature)
        return feature;
    if (call->callee->type == AST_VARIABLE) {
        XaSymbol *sym = xa_parallel_symbol_from_variable_node(ctx, call->callee);
        return xa_parallel_imported_yieldable_feature(ctx, sym, scan->feature_buf,
                                                      sizeof(scan->feature_buf));
    }
    return NULL;
}

static const char *xa_parallel_unknown_effect_call_feature(XaParallelCallbackEffectScan *scan,
                                                           CallExprNode *call) {
    if (!scan || !call || !call->callee)
        return NULL;
    XaInferContext *ctx = scan->ctx;
    const char *feature = xa_parallel_module_member_unknown_effect_feature(
        ctx, call->callee, scan->feature_buf, sizeof(scan->feature_buf));
    if (feature)
        return feature;
    feature = xa_parallel_handle_method_unknown_effect_feature(ctx, call->callee, scan->feature_buf,
                                                               sizeof(scan->feature_buf));
    if (feature)
        return feature;
    if (call->callee->type == AST_VARIABLE) {
        XaSymbol *sym = xa_parallel_symbol_from_variable_node(ctx, call->callee);
        return xa_parallel_imported_unknown_effect_feature(ctx, sym, scan->feature_buf,
                                                           sizeof(scan->feature_buf));
    }
    return NULL;
}

static bool xa_parallel_for_in_iterates_channel(XaInferContext *ctx, AstNode *node) {
    if (!node || node->type != AST_FOR_IN_STMT)
        return false;
    AstNode *collection = node->as.for_in_stmt.collection;
    XrType *collection_type = xa_parallel_node_type(ctx, collection);
    return collection_type && collection_type->kind == XR_KIND_CHANNEL;
}

static bool xa_parallel_function_symbol_has_effect(XaInferContext *ctx, XaSymbol *sym,
                                                   XaSymbol **call_stack, int call_depth,
                                                   bool *out_suspend);
static bool xa_parallel_callback_body_has_effect(XaInferContext *ctx, AstNode *body,
                                                 XaSymbol **call_stack, int call_depth,
                                                 bool *out_suspend);
static XaParallelFunctionValueStatus
xa_parallel_function_value_symbol_status(XaInferContext *ctx, XaSymbol *sym, XaSymbol **call_stack,
                                         int call_depth, bool *out_suspend);
static XaParallelFunctionValueStatus
xa_parallel_function_value_expr_status(XaInferContext *ctx, AstNode *expr, XaSymbol **call_stack,
                                       int call_depth, bool *out_suspend);

static AstNode *xa_parallel_inline_body(AstNode *expr) {
    if (!expr)
        return NULL;
    if (expr->type == AST_FUNCTION_DECL)
        return expr->as.function_decl.body;
    if (expr->type == AST_FUNCTION_EXPR)
        return expr->as.function_expr.body;
    return NULL;
}

static const char *xa_parallel_callback_call_effect_feature(XaParallelCallbackEffectScan *scan,
                                                            CallExprNode *call, bool *is_suspend) {
    if (is_suspend)
        *is_suspend = false;
    if (!call || !call->callee)
        return NULL;

    XaInferContext *ctx = scan ? scan->ctx : NULL;
    if (xa_parallel_call_is_coro_yield(ctx, call)) {
        if (is_suspend)
            *is_suspend = true;
        return "Coro.yield()";
    }

    const char *stdlib_suspend_feature = xa_parallel_stdlib_suspend_call_feature(scan, call);
    if (stdlib_suspend_feature) {
        if (is_suspend)
            *is_suspend = true;
        return stdlib_suspend_feature;
    }

    const char *channel_feature = xa_parallel_channel_blocking_call_feature(ctx, call);
    if (channel_feature) {
        if (is_suspend)
            *is_suspend = true;
        return channel_feature;
    }

    const char *unknown_effect_feature = xa_parallel_unknown_effect_call_feature(scan, call);
    if (unknown_effect_feature) {
        if (is_suspend)
            *is_suspend = true;
        return unknown_effect_feature;
    }

    if (call->callee->type == AST_VARIABLE &&
        xa_parallel_assert_call_name(call->callee->as.variable.name)) {
        const char *name = call->callee->as.variable.name;
        XaSymbol *sym = ctx ? xa_parallel_symbol_from_variable_node(ctx, call->callee) : NULL;
        if (sym && sym->kind == XA_SYM_FUNCTION && sym->is_builtin)
            return name ? name : "assert";
    }

    bool callee_suspend = false;
    AstNode *inline_body = xa_parallel_inline_body(call->callee);
    if (inline_body && xa_parallel_callback_body_has_effect(ctx, inline_body, scan->call_stack,
                                                            scan->call_depth, &callee_suspend)) {
        if (is_suspend)
            *is_suspend = callee_suspend;
        return callee_suspend ? "call to suspendable inline function"
                              : "call to throwing inline function";
    }

    XaSymbol *callee_sym = NULL;
    const char *callee_name = NULL;
    if (call->callee->type == AST_VARIABLE) {
        callee_sym = xa_parallel_symbol_from_variable_node(ctx, call->callee);
        callee_name = call->callee->as.variable.name;
    } else if (call->callee->type == AST_MEMBER_ACCESS) {
        callee_sym = xa_parallel_module_member_symbol(ctx, call->callee);
        MemberAccessNode *ma = &call->callee->as.member_access;
        callee_name = ma->name;
    }

    XaSymbol *target_sym = xa_parallel_import_target_symbol(ctx, callee_sym);
    XaSymbolLinks *target_links =
        target_sym ? xa_analyzer_get_links(ctx->analyzer, target_sym) : NULL;
    if (target_links && target_links->is_extern) {
        if (is_suspend)
            *is_suspend = false;
        snprintf(scan->feature_buf, sizeof(scan->feature_buf), "call to extern function '%s'",
                 callee_name ? callee_name : "?");
        return scan->feature_buf;
    }

    XaParallelFunctionValueStatus status = xa_parallel_function_value_symbol_status(
        ctx, callee_sym, scan->call_stack, scan->call_depth, &callee_suspend);
    if (status == XA_PARALLEL_FN_VALUE_EFFECT) {
        if (is_suspend)
            *is_suspend = callee_suspend;
        const char *kind =
            callee_sym && callee_sym->kind == XA_SYM_FUNCTION ? "function" : "function value";
        snprintf(scan->feature_buf, sizeof(scan->feature_buf), "call to %s %s '%s'",
                 callee_suspend ? "suspendable" : "throwing", kind,
                 callee_name ? callee_name : "?");
        return scan->feature_buf;
    }
    if (status == XA_PARALLEL_FN_VALUE_DYNAMIC) {
        if (is_suspend)
            *is_suspend = true;
        return "call through dynamic function value";
    }

    return NULL;
}

static bool xa_parallel_call_stack_contains(XaSymbol **call_stack, int call_depth, XaSymbol *sym) {
    if (!call_stack || !sym)
        return false;
    for (int i = 0; i < call_depth; i++) {
        if (call_stack[i] == sym)
            return true;
    }
    return false;
}

static bool xa_parallel_function_symbol_has_effect(XaInferContext *ctx, XaSymbol *sym,
                                                   XaSymbol **call_stack, int call_depth,
                                                   bool *out_suspend) {
    if (out_suspend)
        *out_suspend = false;
    sym = xa_parallel_import_target_symbol(ctx, sym);
    if (!ctx || !sym || sym->kind != XA_SYM_FUNCTION || sym->is_builtin)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links && links->is_extern) {
        if (out_suspend)
            *out_suspend = false;
        return true;
    }
    if (xa_parallel_call_stack_contains(call_stack, call_depth, sym))
        return false;
    if (call_depth >= 32)
        return false;

    AstNode *body = xa_parallel_symbol_function_body(ctx, sym);
    if (!body)
        return false;

    call_stack[call_depth] = sym;
    bool result =
        xa_parallel_callback_body_has_effect(ctx, body, call_stack, call_depth + 1, out_suspend);
    call_stack[call_depth] = NULL;
    return result;
}

static bool xa_parallel_symbol_has_function_type(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !ctx->analyzer || !sym)
        return false;
    XrType *type = xa_analyzer_get_type(ctx->analyzer, sym);
    return type && XR_TYPE_IS_FUNCTION(type);
}

static XaParallelFunctionValueStatus
xa_parallel_function_value_symbol_status(XaInferContext *ctx, XaSymbol *sym, XaSymbol **call_stack,
                                         int call_depth, bool *out_suspend) {
    if (out_suspend)
        *out_suspend = false;
    sym = xa_parallel_import_target_symbol(ctx, sym);
    if (!ctx || !sym)
        return XA_PARALLEL_FN_VALUE_SAFE;

    if (sym->kind == XA_SYM_FUNCTION) {
        return xa_parallel_function_symbol_has_effect(ctx, sym, call_stack, call_depth, out_suspend)
                   ? XA_PARALLEL_FN_VALUE_EFFECT
                   : XA_PARALLEL_FN_VALUE_SAFE;
    }

    if (sym->kind != XA_SYM_VARIABLE && sym->kind != XA_SYM_PARAMETER)
        return XA_PARALLEL_FN_VALUE_SAFE;
    if (!xa_parallel_symbol_has_function_type(ctx, sym))
        return XA_PARALLEL_FN_VALUE_SAFE;

    if (sym->kind == XA_SYM_PARAMETER || !sym->is_const)
        return XA_PARALLEL_FN_VALUE_DYNAMIC;
    if (xa_parallel_call_stack_contains(call_stack, call_depth, sym))
        return XA_PARALLEL_FN_VALUE_DYNAMIC;
    if (call_depth >= 32)
        return XA_PARALLEL_FN_VALUE_DYNAMIC;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || !links->const_initializer)
        return XA_PARALLEL_FN_VALUE_DYNAMIC;

    call_stack[call_depth] = sym;
    XaParallelFunctionValueStatus status = xa_parallel_function_value_expr_status(
        ctx, links->const_initializer, call_stack, call_depth + 1, out_suspend);
    call_stack[call_depth] = NULL;
    return status;
}

static XaParallelFunctionValueStatus
xa_parallel_function_value_expr_status(XaInferContext *ctx, AstNode *expr, XaSymbol **call_stack,
                                       int call_depth, bool *out_suspend) {
    if (out_suspend)
        *out_suspend = false;
    if (!ctx || !expr)
        return XA_PARALLEL_FN_VALUE_DYNAMIC;

    AstNode *inline_body = xa_parallel_inline_body(expr);
    if (inline_body) {
        return xa_parallel_callback_body_has_effect(ctx, inline_body, call_stack, call_depth,
                                                    out_suspend)
                   ? XA_PARALLEL_FN_VALUE_EFFECT
                   : XA_PARALLEL_FN_VALUE_SAFE;
    }

    if (expr->type == AST_VARIABLE) {
        XaSymbol *sym = xa_parallel_symbol_from_variable_node(ctx, expr);
        return xa_parallel_function_value_symbol_status(ctx, sym, call_stack, call_depth,
                                                        out_suspend);
    }

    if (expr->type == AST_MEMBER_ACCESS) {
        XaSymbol *sym = xa_parallel_module_member_symbol(ctx, expr);
        if (sym)
            return xa_parallel_function_value_symbol_status(ctx, sym, call_stack, call_depth,
                                                            out_suspend);
        XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
        if (!type)
            type = xa_visit_infer_expr(ctx, expr);
        return type && XR_TYPE_IS_FUNCTION(type) ? XA_PARALLEL_FN_VALUE_DYNAMIC
                                                 : XA_PARALLEL_FN_VALUE_SAFE;
    }

    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    if (!type)
        type = xa_visit_infer_expr(ctx, expr);
    return type && XR_TYPE_IS_FUNCTION(type) ? XA_PARALLEL_FN_VALUE_DYNAMIC
                                             : XA_PARALLEL_FN_VALUE_SAFE;
}

static void xa_parallel_callback_report_effect(XaParallelCallbackEffectScan *scan, AstNode *site,
                                               const char *feature, bool is_suspend) {
    if (!scan || scan->reported || !scan->ctx || !scan->ctx->analyzer || !site || !feature)
        return;

    const char *callback_name = scan->callback_name ? scan->callback_name : "parallel callback";
    XrLocation loc = {.file = scan->ctx->file_path, .line = site->line, .column = site->column};
    char msg[224];
    snprintf(msg, sizeof(msg),
             "%s cannot throw or suspend; %s is not allowed in a parallel callback", callback_name,
             feature);
    xa_analyzer_add_diagnostic(scan->ctx->analyzer, XR_DIAG_SEV_ERROR,
                               is_suspend ? XR_ERR_ANALYZE_AWAIT_TYPE : XR_ERR_ANALYZE, msg, &loc);
    scan->reported = true;
}

static void xa_parallel_callback_effect_scan_pre(AstNode *node, void *ud) {
    XaParallelCallbackEffectScan *scan = (XaParallelCallbackEffectScan *) ud;
    if (!scan || scan->found || scan->reported || !node)
        return;

    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL) {
        scan->nested_function_depth++;
        return;
    }
    if (scan->nested_function_depth > 0)
        return;

    const char *feature = NULL;
    bool is_suspend = false;
    switch (node->type) {
        case AST_AWAIT_EXPR:
            feature = "await";
            is_suspend = true;
            break;
        case AST_GO_EXPR:
            feature = "go expression";
            is_suspend = true;
            break;
        case AST_YIELD_STMT:
            feature = "yield";
            is_suspend = true;
            break;
        case AST_SELECT_STMT:
            feature = "select statement";
            is_suspend = true;
            break;
        case AST_CHAN_SEND:
            feature = "channel send";
            is_suspend = true;
            break;
        case AST_CHAN_RECV:
            feature = "channel recv";
            is_suspend = true;
            break;
        case AST_FOR_IN_STMT:
            if (xa_parallel_for_in_iterates_channel(scan->ctx, node)) {
                feature = "channel iteration";
                is_suspend = true;
            }
            break;
        case AST_THROW_STMT:
            feature = "throw";
            is_suspend = false;
            break;
        case AST_CALL_EXPR:
            feature =
                xa_parallel_callback_call_effect_feature(scan, &node->as.call_expr, &is_suspend);
            break;
        default:
            break;
    }

    if (feature) {
        scan->found = true;
        scan->found_suspend = is_suspend;
        if (scan->report)
            xa_parallel_callback_report_effect(scan, node, feature, is_suspend);
    }
}

static void xa_parallel_callback_effect_scan_post(AstNode *node, void *ud) {
    XaParallelCallbackEffectScan *scan = (XaParallelCallbackEffectScan *) ud;
    if (!scan || !node)
        return;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
         node->type == AST_METHOD_DECL) &&
        scan->nested_function_depth > 0) {
        scan->nested_function_depth--;
    }
}

static bool xa_parallel_callback_body_has_effect(XaInferContext *ctx, AstNode *body,
                                                 XaSymbol **call_stack, int call_depth,
                                                 bool *out_suspend) {
    if (out_suspend)
        *out_suspend = false;
    if (!ctx || !body)
        return false;

    XaParallelCallbackEffectScan scan = {
        .ctx = ctx,
        .callback_name = NULL,
        .call_depth = call_depth,
        .nested_function_depth = 0,
        .report = false,
        .found = false,
        .found_suspend = false,
        .reported = false,
    };
    for (int i = 0; i < call_depth && i < 32; i++)
        scan.call_stack[i] = call_stack ? call_stack[i] : NULL;

    xa_ast_walk(body, xa_parallel_callback_effect_scan_pre, xa_parallel_callback_effect_scan_post,
                &scan);
    if (out_suspend)
        *out_suspend = scan.found_suspend;
    return scan.found;
}

XR_FUNC void xa_parallel_callback_effect_check(XaInferContext *ctx, AstNode *body) {
    if (!ctx || !ctx->in_parallel_callback_body || !body)
        return;
    XaParallelCallbackEffectScan scan = {
        .ctx = ctx,
        .callback_name = ctx->parallel_callback_name,
        .call_depth = 0,
        .nested_function_depth = 0,
        .report = true,
        .found = false,
        .found_suspend = false,
        .reported = false,
    };
    xa_ast_walk(body, xa_parallel_callback_effect_scan_pre, xa_parallel_callback_effect_scan_post,
                &scan);
}

static AstNode *unwrap_grouping(AstNode *node) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node;
}

static XrCallArgAccess xa_super_call_arg_access(const SuperCallNode *call, int index) {
    if (!call || index < 0 || !call->arg_accesses)
        return XR_CALL_ARG_VALUE;
    return call->arg_accesses[index];
}

static XaSymbol *xa_super_call_variable_symbol(XaInferContext *ctx, AstNode *expr) {
    expr = unwrap_grouping(expr);
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    return xa_lookup_visible_symbol(ctx, expr->as.variable.name);
}

static XrType *xa_super_out_direct_variable_type_without_read(XaInferContext *ctx,
                                                              AstNode *arg_node) {
    AstNode *place = unwrap_grouping(arg_node);
    if (!ctx || !ctx->analyzer || !place || place->type != AST_VARIABLE)
        return NULL;
    XaSymbol *sym = xa_super_call_variable_symbol(ctx, place);
    if (!sym)
        return NULL;
    place->as.variable.symbol_id = sym->id;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    XrType *type = xa_analyzer_get_type(ctx->analyzer, sym);
    if (!type && links)
        type = links->type ? links->type : links->declared_type;
    if (!type)
        return NULL;
    xa_analyzer_set_node_type(ctx->analyzer, place, type);
    return type;
}

static XrType *xa_super_out_member_access_type_without_root_read(XaInferContext *ctx,
                                                                 AstNode *arg_node) {
    AstNode *place = unwrap_grouping(arg_node);
    if (!ctx || !ctx->analyzer || !place || place->type != AST_MEMBER_ACCESS)
        return NULL;

    char path[256];
    XaSymbol *root = member_set_out_field_path_symbol(ctx, place, path, sizeof(path));
    if (!root || path[0] == '\0' || root->kind != XA_SYM_PARAMETER ||
        root->passing_mode != XR_PARAM_OUT)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, root);
    if (!links || links->is_definitely_assigned)
        return NULL;

    XrType *obj_type = member_set_out_field_object_type_without_receiver_read(
        ctx, place->as.member_access.object, path);
    if (!obj_type)
        return NULL;

    XrType *type =
        member_set_out_field_type_from_object(ctx, obj_type, place->as.member_access.name);
    if (type)
        xa_analyzer_set_node_type(ctx->analyzer, place, type);
    return type;
}

static XrType *xa_visit_super_arg_for_param_mode(XaInferContext *ctx, AstNode *arg_node,
                                                 XrCallArgAccess access, XrParamMode param_mode) {
    if (access == XR_CALL_ARG_OUT && param_mode == XR_PARAM_OUT) {
        XrType *type = xa_super_out_direct_variable_type_without_read(ctx, arg_node);
        if (type)
            return type;
        type = xa_super_out_member_access_type_without_root_read(ctx, arg_node);
        if (type)
            return type;
    }
    return xa_visit_infer_expr(ctx, arg_node);
}

static void xa_mark_super_out_call_arg_assigned(XaInferContext *ctx, AstNode *arg_node,
                                                XrCallArgAccess access, XrParamMode param_mode) {
    if (access != XR_CALL_ARG_OUT || param_mode != XR_PARAM_OUT)
        return;
    AstNode *place = unwrap_grouping(arg_node);
    if (!ctx || !ctx->analyzer || !place)
        return;
    if (place->type == AST_MEMBER_ACCESS) {
        char path[256];
        XaSymbol *root = member_set_out_field_path_symbol(ctx, place, path, sizeof(path));
        if (!root || path[0] == '\0' || root->kind != XA_SYM_PARAMETER ||
            root->passing_mode != XR_PARAM_OUT)
            return;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, root);
        if (!links || links->is_definitely_assigned)
            return;

        xa_symbol_links_mark_out_field_assigned(links, path);
        if (place->as.member_access.object &&
            place->as.member_access.object->type == AST_MEMBER_ACCESS) {
            char object_path[256];
            XaSymbol *object_root = member_set_out_field_path_symbol(
                ctx, place->as.member_access.object, object_path, sizeof(object_path));
            XrType *object_type =
                xa_analyzer_get_node_type(ctx->analyzer, place->as.member_access.object);
            if (!object_type && object_root == root && object_path[0] != '\0') {
                object_type = member_set_out_field_object_type_without_receiver_read(
                    ctx, place->as.member_access.object, path);
            }
            if (object_root == root && object_path[0] != '\0' && object_type) {
                XaSymbolLinks *class_links = NULL;
                XrClassInfo *object_info = member_set_class_info(ctx, object_type, &class_links);
                (void) class_links;
                xa_symbol_links_mark_out_field_assigned_if_all_direct_fields_assigned_for_class(
                    links, object_path, object_info);
            }
        }
        XrType *root_type = xa_analyzer_get_type(ctx->analyzer, root);
        if (!root_type)
            root_type = links->type ? links->type : links->declared_type;
        xa_symbol_links_mark_out_whole_assigned_if_all_direct_fields_assigned_for_type(
            links, root->name, root_type);
        return;
    }
    if (place->type != AST_VARIABLE)
        return;
    XaSymbol *sym = xa_super_call_variable_symbol(ctx, place);
    if (!sym)
        return;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links)
        return;
    uint32_t end_col =
        place->column + (place->as.variable.name ? strlen(place->as.variable.name) : 0);
    xa_symbol_add_ref(links, place->line, place->column, end_col, true);
    links->is_definitely_assigned = true;
}

static void xa_visit_super_args_without_contract(XaInferContext *ctx, const SuperCallNode *call) {
    if (!ctx || !call)
        return;
    for (int si = 0; si < call->arg_count; si++) {
        if (call->arguments && call->arguments[si])
            xa_visit_infer_expr(ctx, call->arguments[si]);
    }
}

static void xa_visit_super_args_for_function(XaInferContext *ctx, AstNode *node,
                                             XrType *function_type) {
    if (!ctx || !node || node->type != AST_SUPER_CALL || !function_type ||
        !XR_TYPE_IS_FUNCTION(function_type))
        return;
    const SuperCallNode *call = &node->as.super_call;
    int pc = function_type->function.param_count;
    for (int si = 0; si < call->arg_count; si++) {
        AstNode *arg = call->arguments ? call->arguments[si] : NULL;
        if (!arg)
            continue;
        XrParamMode param_mode =
            si < pc ? xr_type_function_param_mode(function_type, si) : XR_PARAM_VALUE;
        XrCallArgAccess access = xa_super_call_arg_access(call, si);
        xa_visit_super_arg_for_param_mode(ctx, arg, access, param_mode);
        if (si < pc) {
            xa_check_arg_access_authorization(ctx, node, arg, access, si, param_mode);
            xa_mark_super_out_call_arg_assigned(ctx, arg, access, param_mode);
        }
    }
}

typedef struct XaContextualIntLiteral {
    bool negative;
    bool signed_valid;
    int64_t signed_value;
    uint64_t unsigned_value;
} XaContextualIntLiteral;

static bool extract_contextual_int_literal(AstNode *node, XaContextualIntLiteral *out_value) {
    node = unwrap_grouping(node);
    if (!node || !out_value)
        return false;

    memset(out_value, 0, sizeof(*out_value));
    if (node->type == AST_LITERAL_INT) {
        out_value->negative = false;
        out_value->unsigned_value = node->as.literal.int_bits;
        if (!node->as.literal.int_overflows_i64) {
            out_value->signed_valid = true;
            out_value->signed_value = node->as.literal.raw_value.int_val;
        }
        return true;
    }

    if (node->type != AST_UNARY_NEG)
        return false;

    AstNode *operand = unwrap_grouping(node->as.unary.operand);
    if (!operand || operand->type != AST_LITERAL_INT)
        return false;

    uint64_t magnitude = operand->as.literal.int_bits;
    out_value->negative = true;
    if (operand->as.literal.int_overflows_i64) {
        if (magnitude == ((uint64_t) INT64_MAX + 1u)) {
            out_value->signed_valid = true;
            out_value->signed_value = INT64_MIN;
        }
        return true;
    }

    int64_t raw = operand->as.literal.raw_value.int_val;
    if (raw == INT64_MIN)
        return true;
    out_value->signed_valid = true;
    out_value->signed_value = -raw;
    return true;
}

static const char *int_range_label(uint8_t native_width) {
    switch (native_width) {
        case XR_NATIVE_I8:
            return "-128..127";
        case XR_NATIVE_U8:
            return "0..255";
        case XR_NATIVE_I16:
            return "-32768..32767";
        case XR_NATIVE_U16:
            return "0..65535";
        case XR_NATIVE_I32:
            return "-2147483648..2147483647";
        case XR_NATIVE_U32:
            return "0..4294967295";
        case XR_NATIVE_I64:
            return "-9223372036854775808..9223372036854775807";
        case XR_NATIVE_U64:
            return "0..18446744073709551615";
        case XR_NATIVE_ISIZE:
            return "target ptrdiff_t range";
        case XR_NATIVE_USIZE:
            return "0..target SIZE_MAX";
        default:
            return NULL;
    }
}

static const char *int_native_width_label(uint8_t native_width) {
    switch (native_width) {
        case XR_NATIVE_I8:
            return "int8";
        case XR_NATIVE_U8:
            return "byte";
        case XR_NATIVE_I16:
            return "int16";
        case XR_NATIVE_U16:
            return "uint16";
        case XR_NATIVE_I32:
            return "int32";
        case XR_NATIVE_U32:
            return "uint32";
        case XR_NATIVE_I64:
            return "int64";
        case XR_NATIVE_U64:
            return "uint64";
        case XR_NATIVE_ISIZE:
            return "intsize";
        case XR_NATIVE_USIZE:
            return "uintsize";
        default:
            return "int";
    }
}

static bool int_literal_fits_native_width(const XaContextualIntLiteral *value,
                                          uint8_t native_width) {
    if (!value)
        return true;
    switch (native_width) {
        case XR_NATIVE_I8:
            return value->signed_valid && value->signed_value >= INT8_MIN &&
                   value->signed_value <= INT8_MAX;
        case XR_NATIVE_U8:
            return !value->negative && value->unsigned_value <= UINT8_MAX;
        case XR_NATIVE_I16:
            return value->signed_valid && value->signed_value >= INT16_MIN &&
                   value->signed_value <= INT16_MAX;
        case XR_NATIVE_U16:
            return !value->negative && value->unsigned_value <= UINT16_MAX;
        case XR_NATIVE_I32:
            return value->signed_valid && value->signed_value >= INT32_MIN &&
                   value->signed_value <= INT32_MAX;
        case XR_NATIVE_U32:
            return !value->negative && value->unsigned_value <= UINT32_MAX;
        case XR_NATIVE_I64:
            return value->signed_valid;
        case XR_NATIVE_U64:
            return !value->negative;
        case XR_NATIVE_ISIZE:
            return value->signed_valid;
        case XR_NATIVE_USIZE:
            return !value->negative;
        default:
            return true;
    }
}

static void format_contextual_int_literal(const XaContextualIntLiteral *value, char *buf,
                                          size_t buf_size) {
    if (!buf || buf_size == 0)
        return;
    if (!value) {
        snprintf(buf, buf_size, "<integer>");
        return;
    }
    if (value->signed_valid) {
        snprintf(buf, buf_size, "%lld", (long long) value->signed_value);
        return;
    }
    if (value->negative)
        snprintf(buf, buf_size, "-%llu", (unsigned long long) value->unsigned_value);
    else
        snprintf(buf, buf_size, "%llu", (unsigned long long) value->unsigned_value);
}

static bool xa_type_name_matches(XrType *type, const char *name) {
    if (!type || !name)
        return false;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_name) {
        return strcmp(type->instance.class_name, name) == 0;
    }
    if (type->kind == XR_KIND_ENUM && type->enum_type.enum_name) {
        return strcmp(type->enum_type.enum_name, name) == 0;
    }
    return false;
}

static bool xa_is_enum_error_type(XrType *type) {
    return type && !XR_TYPE_IS_UNKNOWN(type) && XR_TYPE_IS_ENUM(type);
}

static bool xa_enum_error_type_has_payload(XaAnalyzer *analyzer, XrType *type) {
    if (!xa_is_enum_error_type(type))
        return false;
    if (type->enum_type.layout)
        return !type->enum_type.layout->is_zero_payload;
    const char *enum_name = type->enum_type.enum_name;
    if (!analyzer || !enum_name)
        return false;
    XaSymbol *sym = xa_analyzer_lookup(analyzer, enum_name);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_in_scope(analyzer, enum_name, analyzer->global_scope);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_deep(analyzer, enum_name);
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return links && links->enum_info && links->enum_info->is_payload_enum;
}

static void xa_report_non_enum_catch_type(XaInferContext *ctx, XrCatchClause *cc, XrType *type) {
    if (!ctx || !ctx->analyzer || !cc || !type || XR_TYPE_IS_UNKNOWN(type) ||
        xa_is_enum_error_type(type))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = cc->var_line, .column = cc->var_column};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "catch error type must be an enum error type; use catch panic for runtime faults; "
             "got '%s'",
             xr_type_to_string(type));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_report_freestanding_catch_panic(XaInferContext *ctx, XrCatchClause *cc) {
    if (!ctx || !ctx->analyzer || !cc || !cc->is_panic ||
        !xa_freestanding_profile_enabled(ctx->analyzer))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = cc->var_line, .column = cc->var_column};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                               "freestanding profile rejects catch panic; freestanding panic is "
                               "halt-only via xr_hook_panic",
                               &loc);
}

static XrType *xa_resolve_catch_binding_type(XaInferContext *ctx, XrCatchClause *cc,
                                             bool report_diagnostics) {
    if (!ctx || !cc)
        return xr_type_new_unknown(NULL);

    if (cc->is_panic) {
        if (report_diagnostics)
            xa_report_freestanding_catch_panic(ctx, cc);
        if (cc->type)
            return xr_tref_resolve_in_analyzer(ctx->analyzer, cc->type);
        return xr_type_new_named_instance(ctx->analyzer->isolate, "PanicInfo");
    }

    if (!cc->type)
        return xr_type_new_unknown(ctx->analyzer ? ctx->analyzer->isolate : NULL);

    XrType *type = xr_tref_resolve_in_analyzer(ctx->analyzer, cc->type);
    if (report_diagnostics)
        xa_report_non_enum_catch_type(ctx, cc, type);
    return type ? type : xr_type_new_unknown(ctx->analyzer ? ctx->analyzer->isolate : NULL);
}

static bool xa_catch_pattern_is_bare_type(const XrCatchClause *cc) {
    if (!cc || !cc->pattern || !cc->type || cc->type->kind != XR_TREF_NAMED || !cc->type->name)
        return false;
    AstNode *pattern = cc->pattern;
    if (pattern->type != AST_PATTERN_LITERAL || !pattern->as.pattern_literal.value)
        return false;
    AstNode *value = pattern->as.pattern_literal.value;
    return value->type == AST_VARIABLE && value->as.variable.name &&
           strcmp(value->as.variable.name, cc->type->name) == 0;
}

static void xa_register_catch_pattern_bindings(XaInferContext *ctx, XrCatchClause *cc,
                                               XrType *catch_type) {
    if (!ctx || !cc || !cc->pattern || xa_catch_pattern_is_bare_type(cc) ||
        !xa_pattern_has_binding(cc->pattern))
        return;
    xa_register_pattern_bindings(ctx, cc->pattern, catch_type);
}

static bool xa_is_hashable_interface_type(XrType *type) {
    return type && type->kind == XR_KIND_INTERFACE && type->instance.class_name &&
           strcmp(type->instance.class_name, "Hashable") == 0;
}

static bool xa_type_param_link_has_hashable(XaSymbolLinks *links, const char *name,
                                            bool *out_found) {
    if (out_found)
        *out_found = false;
    if (!links || !name)
        return false;
    int count = xa_symbol_links_get_type_param_count(links);
    for (int i = 0; i < count; i++) {
        const char *param_name = xa_symbol_links_get_type_param_name(links, i);
        if (!param_name || strcmp(param_name, name) != 0)
            continue;
        if (out_found)
            *out_found = true;
        int constraint_count = 0;
        XrType **constraints =
            xa_symbol_links_get_type_param_constraints(links, i, &constraint_count);
        for (int j = 0; j < constraint_count; j++) {
            if (xa_is_hashable_interface_type(constraints ? constraints[j] : NULL))
                return true;
        }
        return false;
    }
    return false;
}

static bool xa_type_param_has_hashable_constraint(XaInferContext *ctx, XaSymbolLinks *generic_links,
                                                  const char *name, bool *out_found) {
    bool found = false;
    if (xa_type_param_link_has_hashable(generic_links, name, &found)) {
        if (out_found)
            *out_found = true;
        return true;
    }
    if (found) {
        if (out_found)
            *out_found = true;
        return false;
    }

    for (XaScope *scope = ctx && ctx->analyzer ? ctx->analyzer->current_scope : NULL; scope;
         scope = scope->parent) {
        if (scope->function_symbol) {
            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, scope->function_symbol);
            if (xa_type_param_link_has_hashable(links, name, &found)) {
                if (out_found)
                    *out_found = true;
                return true;
            }
            if (found) {
                if (out_found)
                    *out_found = true;
                return false;
            }
        }
        if (scope->class_symbol) {
            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, scope->class_symbol);
            if (xa_type_param_link_has_hashable(links, name, &found)) {
                if (out_found)
                    *out_found = true;
                return true;
            }
            if (found) {
                if (out_found)
                    *out_found = true;
                return false;
            }
        }
    }

    if (out_found)
        *out_found = false;
    return false;
}

static XrClassInfo *xa_hashable_class_info_for_type(XaInferContext *ctx, XrType *type) {
    if (!ctx || !ctx->analyzer || !type)
        return NULL;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_ref) {
        return type->instance.class_ref;
    }
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_name) {
        XaSymbol *sym = xa_analyzer_lookup(ctx->analyzer, type->instance.class_name);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
        return links ? links->class_info : NULL;
    }
    return NULL;
}

static bool xa_type_is_builtin_hashable(XrType *type) {
    if (!type)
        return false;
    if (xr_kind_is_primitive(type->kind))
        return true;
    if (type->kind == XR_KIND_NULL || type->kind == XR_KIND_ENUM)
        return true;
    return xr_type_is_named_class(type, "BigInt");
}

static bool xa_method_return_is(XaSymbol *method, XrTypeKind kind) {
    XaSymbolLinks *links = method ? xa_analyzer_get_links(NULL, method) : NULL;
    XrType *type = links ? links->type : NULL;
    return type && XR_TYPE_IS_FUNCTION(type) && type->function.return_type &&
           type->function.return_type->kind == kind;
}

static bool xa_hash_method_valid(XaSymbol *method) {
    if (!method || method->kind != XA_SYM_METHOD || method->is_static || method->is_private)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(NULL, method);
    XrType *type = links ? links->type : NULL;
    return type && XR_TYPE_IS_FUNCTION(type) && type->function.param_count == 0 &&
           xa_method_return_is(method, XR_KIND_INT);
}

static bool xa_eq_method_valid(XaSymbol *method, const char *self_name) {
    if (!method || method->kind != XA_SYM_METHOD || method->is_static || method->is_private)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(NULL, method);
    XrType *type = links ? links->type : NULL;
    if (!type || !XR_TYPE_IS_FUNCTION(type) || type->function.param_count != 1 ||
        !xa_method_return_is(method, XR_KIND_BOOL)) {
        return false;
    }
    XrType *param = xr_type_function_param_type(type, 0);
    return xa_type_name_matches(param, self_name);
}

static void xa_report_derived_hashable_field_contract(XaInferContext *ctx, XrLocation *loc,
                                                      const char *class_name,
                                                      const char *field_name, XrType *field_type) {
    if (!ctx || !ctx->analyzer || !loc)
        return;
    char msg[384];
    snprintf(msg, sizeof(msg),
             "Derived Hash for class '%s' cannot use field '%s' of type '%s'; field type must "
             "satisfy Hashable",
             class_name ? class_name : "?", field_name ? field_name : "?",
             field_type ? xr_type_to_string(field_type) : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_HASHABLE_CONTRACT,
                               msg, loc);
}

static bool xa_type_is_hashable_for_derived_field(XaInferContext *ctx, XrType *type,
                                                  XaSymbolLinks *generic_links,
                                                  const char *owner_name, const char *field_name,
                                                  XrLocation *loc, int depth);

static bool xa_class_derived_hashable_fields_valid(XaInferContext *ctx, XrClassInfo *info,
                                                   XaSymbolLinks *generic_links, XrLocation *loc,
                                                   int depth) {
    if (!ctx || !info)
        return false;
    if (depth >= 8)
        return false;
    for (int i = 0; i < info->field_count; i++) {
        XaSymbol *field = info->fields ? info->fields[i] : NULL;
        if (!field || field->is_static)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, field);
        XrType *field_type = links ? links->type : NULL;
        if (!field_type || XR_TYPE_IS_UNKNOWN(field_type))
            continue;
        if (!xa_type_is_hashable_for_derived_field(ctx, field_type, generic_links, info->name,
                                                   field->name, loc, depth + 1)) {
            return false;
        }
    }
    return true;
}

static bool xa_type_is_hashable_for_derived_field(XaInferContext *ctx, XrType *type,
                                                  XaSymbolLinks *generic_links,
                                                  const char *owner_name, const char *field_name,
                                                  XrLocation *loc, int depth) {
    if (!ctx || !type || XR_TYPE_IS_UNKNOWN(type))
        return true;
    if (xa_type_is_builtin_hashable(type))
        return true;
    if (depth >= 8) {
        xa_report_derived_hashable_field_contract(ctx, loc, owner_name, field_name, type);
        return false;
    }
    if (type->kind == XR_KIND_TYPE_PARAM) {
        bool found = false;
        if (xa_type_param_has_hashable_constraint(ctx, generic_links, type->type_param.name,
                                                  &found))
            return true;
        xa_report_derived_hashable_field_contract(ctx, loc, owner_name, field_name, type);
        return false;
    }
    XrClassInfo *info = xa_hashable_class_info_for_type(ctx, type);
    if (info && info->name) {
        if ((info->derive_flags & (XR_DERIVE_EQ | XR_DERIVE_HASH)) ==
            (XR_DERIVE_EQ | XR_DERIVE_HASH))
            return xa_class_derived_hashable_fields_valid(ctx, info, generic_links, loc, depth + 1);
        XaSymbol *eq = xa_class_info_lookup_member(info, "==");
        XaSymbol *hash = xa_class_info_lookup_member(info, "hash");
        if (xa_eq_method_valid(eq, info->name) && xa_hash_method_valid(hash))
            return true;
    }
    xa_report_derived_hashable_field_contract(ctx, loc, owner_name, field_name, type);
    return false;
}

static void xa_report_hashable_contract(XaInferContext *ctx, XrLocation *loc, const char *type_name,
                                        const char *context, bool has_eq, bool has_hash) {
    if (!ctx || !ctx->analyzer || !loc)
        return;
    const char *name = type_name ? type_name : "<unknown>";
    char missing[192];
    if (!has_eq && !has_hash) {
        snprintf(missing, sizeof(missing),
                 "missing operator==(other: %s) -> bool and hash() -> int", name);
    } else if (!has_eq) {
        snprintf(missing, sizeof(missing), "missing operator==(other: %s) -> bool", name);
    } else {
        snprintf(missing, sizeof(missing), "missing hash() -> int");
    }

    char msg[320];
    if (context && strcmp(context, "implements Hashable") == 0) {
        snprintf(msg, sizeof(msg), "Class '%s' implements Hashable but is %s", name, missing);
    } else {
        snprintf(msg, sizeof(msg), "Type '%s' used as %s must satisfy Hashable: %s", name,
                 context ? context : "Map/Set key", missing);
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_HASHABLE_CONTRACT,
                               msg, loc);
}

static bool xa_validate_hashable_type(XaInferContext *ctx, XrType *type,
                                      XaSymbolLinks *generic_links, const char *context,
                                      XrLocation *loc) {
    if (!ctx || !ctx->analyzer || !loc || !type || XR_TYPE_IS_UNKNOWN(type))
        return true;
    if (xa_type_is_builtin_hashable(type))
        return true;

    if (type->kind == XR_KIND_TYPE_PARAM) {
        const char *name = type->type_param.name;
        bool found = false;
        if (xa_type_param_has_hashable_constraint(ctx, generic_links, name, &found))
            return true;
        char msg[256];
        snprintf(msg, sizeof(msg), "Type parameter '%s' used as %s must be constrained as Hashable",
                 name ? name : "?", context ? context : "Map/Set key");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_HASHABLE_CONTRACT, msg, loc);
        return false;
    }

    XrClassInfo *info = xa_hashable_class_info_for_type(ctx, type);
    if (info && info->name) {
        if ((info->derive_flags & (XR_DERIVE_EQ | XR_DERIVE_HASH)) ==
            (XR_DERIVE_EQ | XR_DERIVE_HASH)) {
            if (!xa_class_derived_hashable_fields_valid(ctx, info, generic_links, loc, 0))
                return false;
            return true;
        }
        XaSymbol *eq = xa_class_info_lookup_member(info, "==");
        XaSymbol *hash = xa_class_info_lookup_member(info, "hash");
        bool has_eq = xa_eq_method_valid(eq, info->name);
        bool has_hash = xa_hash_method_valid(hash);
        if (has_eq && has_hash)
            return true;
        xa_report_hashable_contract(ctx, loc, info->name, context, has_eq, has_hash);
        return false;
    }

    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_name) {
        xa_report_hashable_contract(ctx, loc, type->instance.class_name, context, false, false);
        return false;
    }

    xa_report_hashable_contract(ctx, loc, xr_type_to_string(type), context, false, false);
    return false;
}

XR_FUNC void xa_validate_hashable_contract_for_class(XaInferContext *ctx, AstNode *node,
                                                     XrClassInfo *info) {
    if (!ctx || !info || !info->name)
        return;
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    XaSymbol *eq = xa_class_info_lookup_member(info, "==");
    XaSymbol *hash = xa_class_info_lookup_member(info, "hash");
    bool has_eq = xa_eq_method_valid(eq, info->name);
    bool has_hash = xa_hash_method_valid(hash);
    if (!has_eq || !has_hash)
        xa_report_hashable_contract(ctx, &loc, info->name, "implements Hashable", has_eq, has_hash);
}

XR_FUNC void xa_validate_hashable_key_type(XaInferContext *ctx, XrType *type,
                                           XaSymbolLinks *generic_links, const char *context,
                                           XrLocation *loc) {
    if (!ctx || !ctx->analyzer || !type || !loc)
        return;
    switch (type->kind) {
        case XR_KIND_MAP:
            if (type->is_weak)
                xa_validate_hashable_key_type(ctx, type->map.key_type, generic_links, context, loc);
            else
                xa_validate_hashable_type(ctx, type->map.key_type, generic_links, "Map key", loc);
            xa_validate_hashable_key_type(ctx, type->map.value_type, generic_links, context, loc);
            break;
        case XR_KIND_SET:
            if (type->is_weak)
                xa_validate_hashable_key_type(ctx, type->container.element_type, generic_links,
                                              context, loc);
            else
                xa_validate_hashable_type(ctx, type->container.element_type, generic_links,
                                          "Set element", loc);
            break;
        case XR_KIND_ARRAY:
        case XR_KIND_SPAN:
        case XR_KIND_VIEW:
        case XR_KIND_CHANNEL:
            xa_validate_hashable_key_type(ctx, type->container.element_type, generic_links, context,
                                          loc);
            break;
        case XR_KIND_FIXED_ARRAY:
            xa_validate_hashable_key_type(ctx, type->fixed_array.element_type, generic_links,
                                          context, loc);
            break;
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++)
                xa_validate_hashable_key_type(ctx, type->tuple.element_types[i], generic_links,
                                              context, loc);
            break;
        case XR_KIND_UNION:
            for (int i = 0; i < type->union_type.member_count; i++)
                xa_validate_hashable_key_type(ctx, type->union_type.members[i], generic_links,
                                              context, loc);
            break;
        case XR_KIND_FUNCTION:
            for (int i = 0; i < type->function.param_count; i++)
                xa_validate_hashable_key_type(ctx, xr_type_function_param_type(type, i),
                                              generic_links, context, loc);
            xa_validate_hashable_key_type(ctx, type->function.return_type, generic_links, context,
                                          loc);
            break;
        case XR_KIND_INSTANCE:
            for (int i = 0; i < type->instance.type_arg_count; i++)
                xa_validate_hashable_key_type(ctx, type->instance.type_args[i], generic_links,
                                              context, loc);
            break;
        default:
            break;
    }
}

static void check_contextual_int_literal_range(XaInferContext *ctx, AstNode *node,
                                               XrType *target_type) {
    if (!ctx || !ctx->analyzer || !node || !target_type || !XR_TYPE_IS_INT(target_type))
        return;

    uint8_t native_width = target_type->native_width;
    const char *range = int_range_label(native_width);
    if (!range)
        return;

    XaContextualIntLiteral value;
    if (!extract_contextual_int_literal(node, &value))
        return;
    if (int_literal_fits_native_width(&value, native_width))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char value_buf[64];
    format_contextual_int_literal(&value, value_buf, sizeof(value_buf));
    char msg[256];
    snprintf(msg, sizeof(msg), "Integer literal %s is out of range for type '%s' (expected %s)",
             value_buf, int_native_width_label(native_width), range);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static int object_shape_field_index_local(XrType *type, const char *name) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || !name || !type->object.field_names)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names[i] && strcmp(type->object.field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static XrClassInfo *member_set_class_info(XaInferContext *ctx, XrType *type,
                                          XaSymbolLinks **out_links) {
    if (out_links)
        *out_links = NULL;
    if (!ctx || !ctx->analyzer || !type)
        return NULL;
    if (XR_TYPE_IS_POINTER(type) && type->ptr_is_c_view)
        type = type->container.element_type;
    if (!type || (!XR_TYPE_IS_INSTANCE(type) && !XR_TYPE_IS_CLASS(type)))
        return NULL;

    XrClassInfo *info = type->instance.class_ref;
    const char *class_name = type->instance.class_name;
    if (!class_name && info)
        class_name = info->name;

    XaSymbol *sym = NULL;
    if (class_name) {
        sym = xa_scope_lookup(ctx->analyzer->current_scope, class_name);
        if (!sym)
            sym = xa_scope_lookup(ctx->analyzer->global_scope, class_name);
    }
    if (sym && sym->kind == XA_SYM_CLASS) {
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
        if (out_links)
            *out_links = links;
        if (!info && links)
            info = links->class_info;
    }
    return info;
}

static int member_set_layout_field_index(const XrAggregateLayout *layout, const char *name) {
    if (!layout || !layout->field_names || !name)
        return -1;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        if (layout->field_names[i] && strcmp(layout->field_names[i], name) == 0)
            return (int) i;
    }
    return -1;
}

static bool member_set_out_field_path_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src)
        return false;
    int n = snprintf(dst, dst_size, "%s", src);
    return n >= 0 && (size_t) n < dst_size;
}

static bool member_set_out_field_path_append(char *dst, size_t dst_size, const char *suffix) {
    if (!dst || dst_size == 0 || !suffix)
        return false;
    size_t len = strlen(dst);
    if (len >= dst_size)
        return false;
    int n = snprintf(dst + len, dst_size - len, "%s", suffix);
    return n >= 0 && (size_t) n < dst_size - len;
}

static XaSymbol *member_set_out_field_path_symbol(XaInferContext *ctx, AstNode *expr, char *path,
                                                  size_t path_size) {
    if (!ctx || !ctx->analyzer || !expr || !path || path_size == 0)
        return NULL;
    while (expr && (expr->type == AST_GROUPING || expr->type == AST_FORCE_UNWRAP ||
                    expr->type == AST_AS_EXPR)) {
        if (expr->type == AST_GROUPING)
            expr = expr->as.grouping;
        else if (expr->type == AST_FORCE_UNWRAP)
            expr = expr->as.unary.operand;
        else
            expr = expr->as.as_expr.expr;
    }
    if (!expr)
        return NULL;
    if (expr->type == AST_VARIABLE) {
        XaSymbol *sym = xa_lookup_visible_symbol(ctx, expr->as.variable.name);
        if (!sym || !sym->name || !member_set_out_field_path_copy(path, path_size, sym->name))
            return NULL;
        return sym;
    }
    if (expr->type != AST_MEMBER_ACCESS)
        return NULL;
    XaSymbol *root =
        member_set_out_field_path_symbol(ctx, expr->as.member_access.object, path, path_size);
    if (!root || !expr->as.member_access.name)
        return NULL;
    if (!member_set_out_field_path_append(path, path_size, ".") ||
        !member_set_out_field_path_append(path, path_size, expr->as.member_access.name))
        return NULL;
    return root;
}

static bool member_set_out_field_path_is_same_or_nested(const char *path, const char *prefix) {
    if (!path || !prefix || prefix[0] == '\0')
        return false;
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '.');
}

static XrType *member_set_out_field_type_from_object(XaInferContext *ctx, XrType *obj_type,
                                                     const char *member_name) {
    if (!ctx || !obj_type || !member_name)
        return NULL;
    XaSymbolLinks *class_links = NULL;
    XrClassInfo *class_info = member_set_class_info(ctx, obj_type, &class_links);
    (void) class_links;
    if (class_info) {
        XaSymbol *field = xa_class_info_lookup_member(class_info, member_name);
        XaSymbolLinks *links = field ? xa_analyzer_get_links(ctx->analyzer, field) : NULL;
        XrType *field_type = field ? xa_analyzer_get_type(ctx->analyzer, field) : NULL;
        if (!field_type && links)
            field_type = links->type ? links->type : links->declared_type;
        if (field_type)
            return field_type;
    }
    if (XR_TYPE_HAS_OBJECT_SHAPE(obj_type) && obj_type->object.field_count > 0) {
        int field_idx = object_shape_field_index_local(obj_type, member_name);
        if (field_idx >= 0 && obj_type->object.field_types)
            return obj_type->object.field_types[field_idx];
    }
    return NULL;
}

static XrType *member_set_out_field_object_type_without_receiver_read(XaInferContext *ctx,
                                                                      AstNode *object,
                                                                      const char *target_path) {
    if (!ctx || !object)
        return NULL;
    if (object->type == AST_VARIABLE) {
        char object_path[256];
        XaSymbol *root =
            member_set_out_field_path_symbol(ctx, object, object_path, sizeof(object_path));
        XaSymbolLinks *links = root ? xa_analyzer_get_links(ctx->analyzer, root) : NULL;
        if (!root || !links || links->is_definitely_assigned ||
            !member_set_out_field_path_is_same_or_nested(target_path, object_path))
            return NULL;
        object->as.variable.symbol_id = root->id;
        XrType *type = xa_analyzer_get_type(ctx->analyzer, root);
        if (!type)
            type = links->type ? links->type : links->declared_type;
        if (type)
            xa_analyzer_set_node_type(ctx->analyzer, object, type);
        return type;
    }
    if (object->type == AST_MEMBER_ACCESS) {
        char object_path[256];
        XaSymbol *root =
            member_set_out_field_path_symbol(ctx, object, object_path, sizeof(object_path));
        XaSymbolLinks *links = root ? xa_analyzer_get_links(ctx->analyzer, root) : NULL;
        if (!root || !links || links->is_definitely_assigned ||
            !member_set_out_field_path_is_same_or_nested(target_path, object_path))
            return NULL;
        XrType *parent_type = member_set_out_field_object_type_without_receiver_read(
            ctx, object->as.member_access.object, target_path);
        if (!parent_type)
            return NULL;
        XrType *type =
            member_set_out_field_type_from_object(ctx, parent_type, object->as.member_access.name);
        if (type)
            xa_analyzer_set_node_type(ctx->analyzer, object, type);
        return type;
    }
    return NULL;
}

XR_FUNC XaSymbol *xa_in_param_symbol_for_expr(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, expr->as.variable.name);
    if (sym && sym->kind == XA_SYM_PARAMETER && sym->passing_mode == XR_PARAM_IN)
        return sym;
    return NULL;
}

XR_FUNC bool xa_method_name_mutates_receiver(const char *name) {
    if (!name)
        return false;
    static const char *const mutators[] = {
        "push",       "set",    "appendFrom", "pop",         "shift",        "unshift",
        "reserve",    "resize", "reverse",    "sort",        "store",        "copyFrom",
        "repeatFrom", "fill",   "delete",     "clear",       "add",          "send",
        "recv",       "recvOr", "trySend",    "tryRecv",     "sendTimeout",  "recvTimeout",
        "close",      "cancel", "poll",       "awaitResult", "awaitTimeout", "append",
        "flush",      "tryPop", NULL};
    for (const char *const *p = mutators; *p; p++) {
        if (strcmp(name, *p) == 0)
            return true;
    }
    return false;
}

static XrType *member_set_substitute_field_type(XaInferContext *ctx, XrType *type,
                                                XaSymbolLinks *class_links, XrType *field_type) {
    if (!ctx || !ctx->analyzer || !type || !class_links || !field_type)
        return field_type;
    if (!XR_TYPE_IS_INSTANCE(type) || type->instance.type_arg_count <= 0 ||
        !type->instance.type_args)
        return field_type;

    int param_count = xa_symbol_links_get_type_param_count(class_links);
    if (param_count <= 0 || param_count != type->instance.type_arg_count)
        return field_type;

    const char **param_names = xr_malloc(sizeof(const char *) * (size_t) param_count);
    if (!param_names)
        return field_type;
    for (int i = 0; i < param_count; i++)
        param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);

    XrType *result = xr_type_substitute(ctx->analyzer->isolate, field_type, param_names,
                                        type->instance.type_args, type->instance.type_arg_count);
    xr_free(param_names);
    return result ? result : field_type;
}

/*
 * Recursively convert XR_KIND_CLASS types whose class_name matches a declared
 * type parameter name into XR_KIND_TYPE_PARAM.  The parser has no knowledge of
 * generic scopes, so `T` in `fn add<T>(item: T)` is parsed as CLASS("T").
 * This fixup must run before the function/method type is finalised.
 */
// Cross-TU helper used by xa_visit_collect_class in
// xanalyzer_visitor_decl.c when registering generic-method types.
XrType *resolve_class_to_type_param(XrVMRuntime *X, XrType *type, const char **tp_names,
                                    int tp_count) {
    if (!type || tp_count <= 0)
        return type;

    // Direct match: CLASS("T") → TYPE_PARAM("T")
    if (type->kind == XR_KIND_CLASS && type->instance.class_name) {
        for (int i = 0; i < tp_count; i++) {
            if (tp_names[i] && strcmp(type->instance.class_name, tp_names[i]) == 0)
                return xr_type_new_type_param(X, tp_names[i], i);
        }
    }

    // Recurse into containers
    if ((type->kind == XR_KIND_ARRAY || type->kind == XR_KIND_VIEW || type->kind == XR_KIND_SPAN) &&
        type->container.element_type) {
        XrType *e =
            resolve_class_to_type_param(X, type->container.element_type, tp_names, tp_count);
        if (e != type->container.element_type) {
            if (type->kind == XR_KIND_SPAN)
                return xr_type_new_span(X, e);
            if (type->kind == XR_KIND_VIEW)
                return xr_type_new_view(X, e);
            return xr_type_new_array(X, e);
        }
    }
    if (type->kind == XR_KIND_MAP) {
        XrType *k = resolve_class_to_type_param(X, type->map.key_type, tp_names, tp_count);
        XrType *v = resolve_class_to_type_param(X, type->map.value_type, tp_names, tp_count);
        if (k != type->map.key_type || v != type->map.value_type)
            return xr_type_new_map(X, k, v);
    }
    if (XR_TYPE_IS_FUNCTION(type)) {
        bool changed = false;
        int pc = type->function.param_count;
        XrType **np = pc > 0 ? xr_malloc(sizeof(XrType *) * pc) : NULL;
        for (int i = 0; i < pc; i++) {
            XrType *param_type = xr_type_function_param_type(type, i);
            np[i] = resolve_class_to_type_param(X, param_type, tp_names, tp_count);
            if (np[i] != param_type)
                changed = true;
        }
        XrType *ret =
            resolve_class_to_type_param(X, type->function.return_type, tp_names, tp_count);
        if (ret != type->function.return_type)
            changed = true;
        if (changed) {
            XrType *ft = xr_type_new_function(X, np, pc, ret, type->function.is_variadic);
            ft->function.min_params = type->function.min_params;
            xr_free(np);
            return ft;
        }
        if (np)
            xr_free(np);
    }
    // Generic instance: Box<T> etc.
    if (type->kind == XR_KIND_INSTANCE && type->instance.type_arg_count > 0) {
        bool changed = false;
        int ac = type->instance.type_arg_count;
        XrType **na = xr_malloc(sizeof(XrType *) * ac);
        for (int i = 0; i < ac; i++) {
            na[i] = resolve_class_to_type_param(X, type->instance.type_args[i], tp_names, tp_count);
            if (na[i] != type->instance.type_args[i])
                changed = true;
        }
        if (changed) {
            XrType *r = xr_type_new_generic_instance(X, type->instance.class_name,
                                                     type->instance.class_ref, na, ac);
            r->semantic_type_id = type->semantic_type_id;
            return r;
        }
        xr_free(na);
    }
    return type;
}

XR_FUNC void xa_set_function_type_params_from_ast(XaInferContext *ctx, XrType *fn_type,
                                                  XrGenericParam **type_params, int count) {
    if (!ctx || !ctx->analyzer || !fn_type || !XR_TYPE_IS_FUNCTION(fn_type) || count <= 0 ||
        !type_params)
        return;

    const char **names = xr_malloc(sizeof(const char *) * count);
    XrType ***constraint_lists = xr_malloc(sizeof(XrType **) * count);
    int *constraint_counts = xr_malloc(sizeof(int) * count);
    if (!names || !constraint_lists || !constraint_counts) {
        xr_free(names);
        xr_free(constraint_lists);
        xr_free(constraint_counts);
        return;
    }

    // Collect all names first so resolve_class_to_type_param can safely
    // read the full names array when checking constraints for early params.
    for (int i = 0; i < count; i++) {
        XrGenericParam *gp = type_params[i];
        names[i] = gp ? gp->name : NULL;
    }

    for (int i = 0; i < count; i++) {
        XrGenericParam *gp = type_params[i];
        int cn = gp ? gp->constraint_count : 0;
        constraint_counts[i] = cn;
        constraint_lists[i] = NULL;
        if (cn > 0 && gp && gp->constraints) {
            constraint_lists[i] = xr_malloc(sizeof(XrType *) * cn);
            if (constraint_lists[i]) {
                for (int j = 0; j < cn; j++) {
                    constraint_lists[i][j] =
                        gp->constraints[j]
                            ? xr_tref_resolve_in_analyzer(ctx->analyzer, gp->constraints[j])
                            : NULL;
                    constraint_lists[i][j] = resolve_class_to_type_param(
                        ctx->analyzer->isolate, constraint_lists[i][j], names, count);
                    xa_reject_error_type_success_type(ctx->analyzer, constraint_lists[i][j],
                                                      "generic constraint", gp->name, 0, 0);
                }
            } else {
                constraint_counts[i] = 0;
            }
        }
    }

    xr_type_set_function_type_params(ctx->analyzer->isolate, fn_type, names, constraint_lists,
                                     constraint_counts, count);

    for (int i = 0; i < count; i++) {
        if (constraint_lists[i])
            xr_free(constraint_lists[i]);
    }
    xr_free(names);
    xr_free(constraint_lists);
    xr_free(constraint_counts);
}

// Check null→T and T?→T assignment errors.
// Returns true if an error was emitted (caller should still call xr_type_assignable).
bool xa_check_null_safety(XaAnalyzer *analyzer, XrType *target, XrType *source,
                          const char *context_msg, XrLocation *loc) {
    if (!target || !source)
        return false;
    if (XR_TYPE_IS_UNKNOWN(target) || XR_TYPE_IS_UNKNOWN(source))
        return false;

    // Value types (structs) can never be null
    if (XR_TYPE_IS_NULL(source) && target->is_value_type) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%s: cannot assign 'null' to value type '%s' (structs are non-nullable)",
                 context_msg, xr_type_to_string(target));
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                                   loc);
        return true;
    }

    // Null-safety violations are always errors (xray is a strongly-typed language).
    XrDiagSeverity null_sev = XR_DIAG_SEV_ERROR;

    // null -> T. Reject unless the target is nullable (`T?`) or a
    // type whose value domain natively contains null (currently
    // Json). The `intrinsically_includes_null` helper centralises
    // that second predicate; see its doc comment for the rationale.
    if (XR_TYPE_IS_NULL(source) && !target->is_nullable &&
        !xr_type_intrinsically_includes_null(target)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: cannot assign 'null' to non-nullable type '%s'",
                 context_msg, xr_type_to_string(target));
        xa_analyzer_add_diagnostic(analyzer, null_sev, XR_ERR_ANALYZE_TYPE_MISMATCH, msg, loc);
        return true;
    }

    // T? → T (nullable source to non-nullable target, without narrowing)
    if (source->is_nullable && !target->is_nullable) {
        XrType *src_base = xr_type_non_nullable(analyzer->isolate, source);
        if (src_base && xr_type_assignable(target, src_base)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "%s: cannot assign '%s' to '%s' without null check. "
                     "Use '?\?' default or '!' unwrap",
                     context_msg, xr_type_to_string(source), xr_type_to_string(target));
            xa_analyzer_add_diagnostic(analyzer, null_sev, XR_ERR_ANALYZE_TYPE_MISMATCH, msg, loc);
            return true;
        }
    }

    return false;
}

// Helper: recursively extract a type parameter from an argument type
// e.g., match(Array<T>, Array<int>, "T") -> int
//        match(Map<K,V>, Map<string,int>, "K") -> string
XrType *xa_infer_type_param_from_arg(XrType *param_type, XrType *arg_type, const char *tp_name,
                                     int depth) {
    if (!param_type || !arg_type || depth > 8)
        return NULL;

    // Direct match: T vs int -> T = int
    if ((param_type->kind == XR_KIND_TYPE_PARAM) && param_type->type_param.name &&
        strcmp(param_type->type_param.name, tp_name) == 0) {
        return arg_type;
    }

    // Array<T> vs Array<int> -> T = int
    if (XR_TYPE_IS_ARRAY(param_type) && XR_TYPE_IS_ARRAY(arg_type)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // Span<T> vs Span<int> -> T = int. Owner-to-span conversion is produced
    // only by target-typed slice/view-producing expressions.
    if (XR_TYPE_IS_SPAN(param_type) && XR_TYPE_IS_SPAN(arg_type)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // View<T> vs View<int> -> T = int. Owner-to-view conversion is produced by
    // target-typed slice expressions, not by generic argument matching.
    if (XR_TYPE_IS_VIEW(param_type) && XR_TYPE_IS_VIEW(arg_type)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // Set<T> vs Set<int> -> T = int
    if ((param_type->kind == XR_KIND_SET) && (arg_type->kind == XR_KIND_SET)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // Map<K, V> vs Map<string, int> -> K = string, V = int
    if (XR_TYPE_IS_MAP(param_type) && XR_TYPE_IS_MAP(arg_type)) {
        XrType *from_key = xa_infer_type_param_from_arg(param_type->map.key_type,
                                                        arg_type->map.key_type, tp_name, depth + 1);
        if (from_key)
            return from_key;
        return xa_infer_type_param_from_arg(param_type->map.value_type, arg_type->map.value_type,
                                            tp_name, depth + 1);
    }

    // Task<T> vs Task<int> -> T = int
    if (xr_type_is_named_class(param_type, "Task") && xr_type_is_named_class(arg_type, "Task")) {
        XrType *pt =
            (param_type->instance.type_arg_count > 0) ? param_type->instance.type_args[0] : NULL;
        XrType *at =
            (arg_type->instance.type_arg_count > 0) ? arg_type->instance.type_args[0] : NULL;
        return xa_infer_type_param_from_arg(pt, at, tp_name, depth + 1);
    }

    // Channel<T> vs Channel<int> -> T = int
    if ((param_type->kind == XR_KIND_CHANNEL) && (arg_type->kind == XR_KIND_CHANNEL)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // fn(T): U vs fn(int): string -> T = int, U = string
    if (XR_TYPE_IS_FUNCTION(param_type) && XR_TYPE_IS_FUNCTION(arg_type)) {
        int pc = param_type->function.param_count;
        int ac = arg_type->function.param_count;
        int min = pc < ac ? pc : ac;
        for (int i = 0; i < min; i++) {
            XrType *r = xa_infer_type_param_from_arg(xr_type_function_param_type(param_type, i),
                                                     xr_type_function_param_type(arg_type, i),
                                                     tp_name, depth + 1);
            if (r)
                return r;
        }
        if (param_type->function.return_type && arg_type->function.return_type) {
            return xa_infer_type_param_from_arg(param_type->function.return_type,
                                                arg_type->function.return_type, tp_name, depth + 1);
        }
    }

    return NULL;
}

// Helper: apply generic type substitution for a call expression
// Builds param_names from symbol links, resolves actual types (explicit or inferred),
// then substitutes into return_type. Returns the substituted type.
XrType *xa_substitute_generic_call(XaInferContext *ctx, XaSymbolLinks *links, XrType *callee_type,
                                   XrType *return_type, CallExprNode *call, int arg_count,
                                   XrType **effective_arg_types) {
    XR_DCHECK(ctx != NULL, "substitute_generic_call: NULL ctx");
    XR_DCHECK(links != NULL, "substitute_generic_call: NULL links");
    int type_param_count = xa_symbol_links_get_type_param_count(links);
    if (type_param_count <= 0 || !return_type)
        return return_type;

    const char **param_names = xr_malloc(sizeof(const char *) * type_param_count);
    if (!param_names)
        return return_type;
    for (int i = 0; i < type_param_count; i++) {
        param_names[i] = xa_symbol_links_get_type_param_name(links, i);
    }

    XrType **actual_types = NULL;
    int actual_count = 0;
    bool inferred = false;

    if (call->type_arg_count > 0) {
        actual_types = xr_malloc(sizeof(XrType *) * (size_t) call->type_arg_count);
        if (!actual_types) {
            xr_free(param_names);
            return return_type;
        }
        for (int i = 0; i < call->type_arg_count; i++) {
            actual_types[i] = call->type_args[i]
                                  ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[i])
                                  : xr_type_new_unknown(NULL);
        }
        actual_count = call->type_arg_count;
        inferred = true; /* mark for free */
    } else {
        actual_types = xr_malloc(sizeof(XrType *) * type_param_count);
        if (!actual_types) {
            xr_free(param_names);
            return return_type;
        }
        actual_count = type_param_count;
        inferred = true;

        int param_count = callee_type->function.param_count;

        for (int i = 0; i < type_param_count; i++) {
            actual_types[i] = NULL;
            const char *tp_name = param_names[i];
            for (int j = 0; j < param_count && j < arg_count; j++) {
                XrType *pt = xr_type_function_param_type(callee_type, j);
                XrType *at = effective_arg_types ? effective_arg_types[j] : NULL;
                if (!at && j < call->arg_count) {
                    // Use cached type if available (avoid re-evaluating lambdas
                    // which would lose callback context and trigger duplicate warnings)
                    XrType *cached = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[j]);
                    at = cached ? cached : xa_visit_infer_expr(ctx, call->arguments[j]);
                }
                if (pt && at) {
                    actual_types[i] = xa_infer_type_param_from_arg(pt, at, tp_name, 0);
                    if (actual_types[i])
                        break;
                }
            }
        }
    }

    bool poisoned_type_arg = false;
    for (int i = 0; i < actual_count; i++) {
        if (xa_reject_error_type_success_type(ctx->analyzer, actual_types[i],
                                              "generic type argument", "function", 0, 0))
            poisoned_type_arg = true;
    }
    if (poisoned_type_arg) {
        xr_free(param_names);
        if (inferred && actual_types)
            xr_free(actual_types);
        return xr_type_new_error(NULL);
    }

    if (actual_count > 0) {
        return_type = xr_type_substitute(ctx->analyzer->isolate, return_type, param_names,
                                         actual_types, actual_count);
    }

    xr_free(param_names);
    if (inferred && actual_types)
        xr_free(actual_types);
    return return_type;
}

// Check if a function body contains any 'return <expr>' (non-void return).
// Does NOT recurse into nested functions/lambdas.
XR_FUNC bool xa_body_has_return_expr(AstNode *node) {
    if (!node)
        return false;
    switch (node->type) {
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            return (ret->value_count > 0 && ret->values && ret->values[0]);
        }
        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            for (int i = 0; i < block->count; i++) {
                if (xa_body_has_return_expr(block->statements[i]))
                    return true;
            }
            return false;
        }
        case AST_IF_STMT:
            return xa_body_has_return_expr(node->as.if_stmt.then_branch) ||
                   xa_body_has_return_expr(node->as.if_stmt.else_branch);
        case AST_WHILE_STMT:
            return xa_body_has_return_expr(node->as.while_stmt.body);
        case AST_FOR_STMT:
            return xa_body_has_return_expr(node->as.for_stmt.body);
        case AST_FOR_IN_STMT:
            return xa_body_has_return_expr(node->as.for_in_stmt.body);
        case AST_TRY_CATCH: {
            if (xa_body_has_return_expr(node->as.try_catch.try_body))
                return true;
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc && xa_body_has_return_expr(cc->body))
                    return true;
            }
            return false;
        }
        case AST_MATCH_EXPR: {
            MatchExprNode *m = &node->as.match_expr;
            for (int i = 0; i < m->arm_count; i++) {
                if (m->arms[i] && m->arms[i]->type == AST_MATCH_ARM) {
                    if (xa_body_has_return_expr(m->arms[i]->as.match_arm.body))
                        return true;
                }
            }
            return false;
        }
        // Do NOT recurse into nested functions/lambdas
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return false;
        default:
            return false;
    }
}

XR_FUNC void xa_visit_add_symbol_checked(XaInferContext *ctx, XaSymbol *symbol, int line) {
    if (!ctx || !ctx->analyzer || !symbol)
        return;
    XaScope *scope = ctx->analyzer->current_scope;
    if (!scope)
        return;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    if (links && !links->file_path)
        links->file_path = ctx->file_path;

    XaSymbol *existing = symbol->name ? xa_scope_lookup_local(scope, symbol->name) : NULL;
    if (existing) {
        bool same_source_symbol = existing->kind == symbol->kind &&
                                  existing->location.line == symbol->location.line &&
                                  existing->location.column == symbol->location.column;
        if (!same_source_symbol) {
            char msg[256];
            /* Result / Ordering are canonical builtin prelude enums bound to a
             * single type identity; they are reserved and cannot be
             * redeclared (the lowerer always resolves them to their builtin
             * slot, so allowing a redefinition would silently mis-resolve). */
            if (existing->is_builtin && existing->kind == XA_SYM_ENUM)
                snprintf(msg, sizeof(msg),
                         "'%s' is a builtin prelude enum and cannot be redeclared", symbol->name);
            else
                snprintf(msg, sizeof(msg), "Symbol '%s' is redefined in the same scope",
                         symbol->name);
            XrLocation loc = {.file = ctx->file_path,
                              .line = line > 0 ? line : (int) symbol->location.line,
                              .column = (int) symbol->location.column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_CMP_REDEFINED_VAR,
                                       msg, &loc);
        }
    }
    xa_scope_add_symbol(scope, symbol);
}

XR_FUNC XaSymbol *xa_visit_bind_parameter_symbol(XaInferContext *ctx, XrParamNode *param,
                                                 int fallback_line) {
    if (!ctx || !ctx->analyzer || !ctx->analyzer->current_scope || !param || !param->name)
        return NULL;

    XaSymbol *symbol = NULL;
    if (param->symbol_id != 0) {
        XaSymbol *bound = xa_scope_lookup_by_id(ctx->analyzer->global_scope, param->symbol_id);
        if (bound && bound->scope == ctx->analyzer->current_scope &&
            bound->kind == XA_SYM_PARAMETER && bound->name &&
            strcmp(bound->name, param->name) == 0) {
            symbol = bound;
        }
    }

    if (!symbol) {
        symbol = xa_symbol_new(param->name, XA_SYM_PARAMETER);
        if (!symbol)
            return NULL;
        symbol->location.line = param->line > 0 ? param->line : fallback_line;
        symbol->location.column = param->column;
        xa_visit_add_symbol_checked(ctx, symbol, 0);
        param->symbol_id = symbol->id;
    }

    symbol->passing_mode = param->passing_mode;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    if (links) {
        links->is_definitely_assigned = param->passing_mode != XR_PARAM_OUT;
        xa_symbol_links_restore_out_field_da_paths(links, NULL);
    }
    return symbol;
}

/* ============================================================================
 * Pass 1: Symbol Collection
 * ============================================================================
 * Collects all symbols (functions, classes, variables) before type inference.
 * Uses two-phase approach for functions/classes to support mutual recursion.
 * ========================================================================== */

static void xa_visit_precollect_const_decl(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || node->type != AST_CONST_DECL)
        return;
    VarDeclNode *var = &node->as.var_decl;
    if (!var->name || var->symbol_id != 0)
        return;
    if (xa_scope_lookup_local(ctx->analyzer->current_scope, var->name))
        return;

    XaSymbol *sym = xa_symbol_new(var->name, XA_SYM_VARIABLE);
    sym->location.line = node->line;
    sym->is_const = true;
    sym->is_exported = node->is_exported;
    sym->is_readonly_binding = true;
    sym->is_rebindable = false;
    xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
    var->symbol_id = sym->id;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        links->const_initializer = var->initializer;
        links->file_path = ctx->file_path;
        AstNode *initializer = xa_discard_lint_unwrap_expr(var->initializer);
        links->is_definitely_assigned = initializer && initializer->type != AST_FUNCTION_EXPR;
    }
}

static XaSymbol *xa_visit_predeclare_type_alias(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || node->type != AST_TYPE_ALIAS)
        return NULL;
    TypeAliasNode *ta = &node->as.type_alias;
    if (!ta->name)
        return NULL;

    XaSymbol *existing = xa_scope_lookup_local(ctx->analyzer->current_scope, ta->name);
    if (existing) {
        if (existing->kind == XA_SYM_TYPE_ALIAS) {
            ta->symbol_id = existing->id;
            existing->type_alias_node = node;
            existing->is_exported = node->is_exported;
            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, existing);
            if (links && !links->file_path)
                links->file_path = ctx->file_path;
            return existing;
        }
        return existing;
    }

    XaSymbol *sym = xa_symbol_new(ta->name, XA_SYM_TYPE_ALIAS);
    if (!sym)
        return NULL;
    sym->location.line = node->line;
    sym->location.column = node->column;
    sym->is_const = true;
    sym->is_exported = node->is_exported;
    sym->type_alias_node = node;
    xa_visit_add_symbol_checked(ctx, sym, 0);
    ta->symbol_id = sym->id;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        links->file_path = ctx->file_path;
        if (ta->type_param_count > 0 && ta->type_params) {
            const char **type_param_names =
                (const char **) xr_calloc((size_t) ta->type_param_count, sizeof(const char *));
            if (type_param_names) {
                for (int i = 0; i < ta->type_param_count; i++)
                    type_param_names[i] = ta->type_params[i] ? ta->type_params[i]->name : NULL;
                xa_symbol_links_set_type_params(links, type_param_names, NULL, NULL,
                                                ta->type_param_count);
                xr_free(type_param_names);
            }
        }
    }
    return sym;
}

static void xa_visit_collect_import(XaInferContext *ctx, AstNode *node);

static void xa_visit_predeclare_class_decl(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return;
    bool is_struct_decl = node->type == AST_STRUCT_DECL;
    bool is_union_decl = node->type == AST_UNION_DECL;
    if (node->type != AST_CLASS_DECL && !is_struct_decl && !is_union_decl)
        return;

    ClassDeclNode *cls = (node->type == AST_CLASS_DECL) ? &node->as.class_decl
                         : is_struct_decl               ? &node->as.struct_decl
                                                        : &node->as.union_decl;
    if (!cls->name || cls->symbol_id != 0)
        return;

    XaSymbol *sym = xa_symbol_new(cls->name, XA_SYM_CLASS);
    sym->location.line = node->line;
    sym->is_exported = node->is_exported;
    xa_visit_add_symbol_checked(ctx, sym, 0);
    cls->symbol_id = sym->id;

    XrClassInfo *info = xa_class_info_new(cls->name);
    info->explicit_final = cls->explicit_final;
    info->is_extern_layout = cls->is_extern_layout;
    info->location =
        (XrLocation) {.file = ctx->file_path, .line = node->line, .column = node->column};
    if (cls->super_name)
        info->base_name = xr_strdup(cls->super_name);

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->class_info = info;
    links->type = xr_type_new_class(ctx->analyzer->isolate, cls->name);
    links->type->instance.class_ref = info;
    if (is_struct_decl || is_union_decl)
        links->type->is_value_type = true;
}

static void xa_visit_predeclare_enum_decl(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || node->type != AST_ENUM_DECL)
        return;
    EnumDeclNode *edecl = &node->as.enum_decl;
    if (!edecl->name || edecl->symbol_id != 0)
        return;

    XaSymbol *sym = xa_symbol_new(edecl->name, XA_SYM_ENUM);
    sym->location.line = node->line;
    sym->is_const = true;
    sym->is_exported = node->is_exported;
    xa_visit_add_symbol_checked(ctx, sym, 0);
    edecl->symbol_id = sym->id;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->type = xr_type_new_enum(ctx->analyzer->isolate, edecl->name);
    links->declared_type = links->type;
    links->class_info = xa_class_info_new(edecl->name);
    if (edecl->type_param_count > 0 && edecl->type_params) {
        const char **type_param_names =
            xr_malloc(sizeof(const char *) * (size_t) edecl->type_param_count);
        if (type_param_names) {
            for (int i = 0; i < edecl->type_param_count; i++)
                type_param_names[i] = edecl->type_params[i] ? edecl->type_params[i]->name : NULL;
            xa_symbol_links_set_type_params(links, type_param_names, NULL, NULL,
                                            edecl->type_param_count);
            xr_free(type_param_names);
        }
    }
}

// xa_visit_collect_function_decl_only / xa_visit_collect_function_body
// Cross-TU: also called from xa_visit_collect_function_body() in
// xanalyzer_visitor_decl.c for nested function bodies.
void xa_visit_collect_statements_with_hoisting(XaInferContext *ctx, AstNode **stmts, int count) {
    // Phase -0.5: imports must be visible before signature and field type
    // resolution. Strict type lookup cannot depend on the old unknown class-name
    // fallback for imported declaration types.
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_IMPORT_STMT)
            xa_visit_collect_import(ctx, stmt);
    }

    // Phase 0: register every interface declaration first so that any class
    // body parsed in Phase 1 can resolve `implements Foo` against a real
    // symbol (Foo could appear before or after the class in source order).
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_INTERFACE_DECL) {
            xa_visit_collect_interface(ctx, stmt);
        }
    }

    // Phase 0.5: pre-register const initializers so signatures and field types
    // can use [T; N] where N is a source-level const.
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_CONST_DECL) {
            xa_visit_precollect_const_decl(ctx, stmt);
        }
    }

    // Phase 0.75: predeclare aggregate/class/enum names before function
    // signatures are resolved. This keeps legitimate same-module forward type
    // references out of the generic "undefined type" path.
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_CLASS_DECL || stmt->type == AST_STRUCT_DECL ||
            stmt->type == AST_UNION_DECL) {
            xa_visit_predeclare_class_decl(ctx, stmt);
        }
        if (stmt->type == AST_ENUM_DECL) {
            xa_visit_predeclare_enum_decl(ctx, stmt);
        }
    }

    // Phase 0.8: predeclare all type alias names before resolving alias RHS or
    // signatures. Alias expansion still requires a real symbol and never falls
    // back to an unknown class-name placeholder.
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_TYPE_ALIAS) {
            xa_visit_predeclare_type_alias(ctx, stmt);
        }
    }

    // Phase 1: Collect all function/class/enum declarations first (hoisting)
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_FUNCTION_DECL) {
            xa_visit_collect_function_decl_only(ctx, stmt);
        } else if (stmt->type == AST_CLASS_DECL || stmt->type == AST_STRUCT_DECL ||
                   stmt->type == AST_UNION_DECL) {
            xa_visit_collect_class(ctx, stmt);
        } else if (stmt->type == AST_ENUM_DECL) {
            xa_visit_collect(ctx, stmt);
        }
    }

    // Phase 2: Collect function bodies and other declarations
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_FUNCTION_DECL) {
            xa_visit_collect_function_body(ctx, stmt);
        } else if (stmt->type != AST_IMPORT_STMT && stmt->type != AST_CLASS_DECL &&
                   stmt->type != AST_STRUCT_DECL && stmt->type != AST_UNION_DECL &&
                   stmt->type != AST_ENUM_DECL && stmt->type != AST_EXPORT_STMT) {
            /* Bare block statements need a scope so inner var/const
             * declarations get distinct symbol_ids from outer variables
             * with the same name (variable shadowing).  Matches Pass 2's
             * xa_visit_block_stmt which enters BLOCK_SCOPE. */
            if (stmt->type == AST_BLOCK) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, stmt);
                xa_visit_collect(ctx, stmt);
                xa_analyzer_exit_scope(ctx->analyzer);
            } else {
                xa_visit_collect(ctx, stmt);
            }
        }
    }
}

/* Try to resolve an import target's exported semantic symbols from the module graph.
 * Returns the target module's export-symbol hashmap, or NULL if unavailable. */
XR_FUNC XrHashMap *resolve_graph_export_symbols(XaAnalyzer *analyzer, const char *module_name,
                                                bool is_quoted) {
    XrModuleGraph *graph = (XrModuleGraph *) analyzer->graph;
    if (!graph)
        return NULL;

    /* Resolve the import specifier to a canonical ID */
    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(graph->resolver, module_name, !is_quoted,
                                        analyzer->current_file, &mid, &err);
    xr_free(err);
    if (rc != 0)
        return NULL;

    /* Find the target module in the graph */
    int idx = xr_module_graph_find(graph, mid.canonical);
    xr_module_id_cleanup(&mid);
    if (idx < 0)
        return NULL;

    if (graph->specs[idx].export_symbols_invalid)
        return NULL;

    return graph->specs[idx].export_symbols;
}

// Helper: collect import statement (register module variable in symbol table)
static void xa_visit_collect_import(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ImportStmtNode *import = &node->as.import_stmt;
    if (xa_freestanding_profile_enabled(ctx->analyzer) &&
        (!import->is_quoted || xa_freestanding_stdlib_module_known(import->module_name)) &&
        !xa_freestanding_stdlib_module_allowed(import->module_name)) {
        char feature[160];
        snprintf(feature, sizeof(feature), "stdlib module '%s'",
                 import->module_name ? import->module_name : "?");
        xa_freestanding_report_unavailable(
            ctx, node, feature,
            "only prelude, math, and mem are in the initial freestanding allowlist");
    }

    // For whole module import: import math or import math as m
    if (import->member_count == 0) {
        const char *var_name = import->alias ? import->alias : import->module_name;

        XaSymbol *sym = xa_symbol_new(var_name, XA_SYM_MODULE);
        if (sym) {
            sym->location.line = node->line;
            xa_visit_add_symbol_checked(ctx, sym, 0);
            import->symbol_id = sym->id;

            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
            if (links) {
                // Store actual module name for type lookup (handles aliasing)
                links->module_name = import->module_name;
                links->type = xr_type_new_unknown(NULL);
                links->declared_type = links->type;
            }
        }
    } else {
        // For selective import: import { a, b } from "module"
        XrHashMap *graph_exports =
            resolve_graph_export_symbols(ctx->analyzer, import->module_name, import->is_quoted);

        for (int i = 0; i < import->member_count; i++) {
            ImportMember *member = &import->members[i];
            if (xa_freestanding_profile_enabled(ctx->analyzer) &&
                (!import->is_quoted || xa_freestanding_stdlib_module_known(import->module_name)) &&
                !xa_freestanding_stdlib_member_allowed(import->module_name, member->name)) {
                char feature[192];
                snprintf(feature, sizeof(feature), "%s.%s",
                         import->module_name ? import->module_name : "?", member->name);
                xa_freestanding_report_unavailable(
                    ctx, node, feature,
                    xa_freestanding_stdlib_member_reject_suggestion(import->module_name));
            }
            const char *local_name = member->alias ? member->alias : member->name;
            XaSymbol *export_sym =
                graph_exports ? (XaSymbol *) xr_hashmap_get(graph_exports, member->name) : NULL;

            // Register each imported member as its exported semantic kind.
            XaSymbol *sym =
                xa_symbol_new(local_name, export_sym ? export_sym->kind : XA_SYM_IMPORT);
            if (sym) {
                sym->is_imported = true;
                sym->is_const = true;
                if (export_sym) {
                    sym->is_static = export_sym->is_static;
                    sym->is_private = export_sym->is_private;
                    sym->is_protected = export_sym->is_protected;
                    sym->is_override = export_sym->is_override;
                    sym->is_shared = export_sym->is_shared;
                    sym->is_owned = export_sym->is_owned;
                    sym->is_builtin = export_sym->is_builtin;
                    sym->mutates_receiver = export_sym->mutates_receiver;
                    sym->passing_mode = export_sym->passing_mode;
                    sym->alias_type = export_sym->alias_type;
                }
                sym->location.line = node->line;
                xa_visit_add_symbol_checked(ctx, sym, 0);
                member->symbol_id = sym->id;

                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links) {
                    XrType *member_type = NULL;
                    const char *builtin_sig = NULL;

                    // Priority 1: resolve from graph exports (user modules)
                    if (export_sym) {
                        xa_symbol_links_copy_export_metadata(links, &export_sym->links);
                        member_type = links->type;
                    }

                    // Priority 2: resolve from builtin module signatures (stdlib)
                    if (!member_type) {
                        builtin_sig =
                            xa_builtin_get_module_func_signature(import->module_name, member->name);
                        if (builtin_sig) {
                            if (builtin_sig[0] == ':') {
                                const char *type_str = builtin_sig + 1;
                                while (*type_str == ' ')
                                    type_str++;
                                member_type =
                                    xa_builtin_parse_type_string(ctx->analyzer->isolate, type_str);
                            } else {
                                member_type = xa_builtin_parse_full_signature(
                                    ctx->analyzer->isolate, builtin_sig);
                            }
                        }
                    }

                    if (!export_sym && !builtin_sig)
                        xa_report_unknown_stdlib_member(ctx, node, import->module_name,
                                                        member->name);

                    if (!export_sym) {
                        links->type = member_type ? member_type : xr_type_new_unknown(NULL);
                        links->declared_type = links->type;
                    }
                    links->module_name = import->module_name;
                    links->import_member_name = member->name;
                }
            }
        }
    }
}

static XrType *enum_method_this_type(XaInferContext *ctx, const char *enum_name) {
    return enum_name ? xr_type_new_enum(ctx->analyzer->isolate, enum_name)
                     : xr_type_new_unknown(NULL);
}

static void xa_visit_collect_enum_method(XaInferContext *ctx, XaSymbol *enum_sym, XrClassInfo *info,
                                         AstNode *method) {
    if (!ctx || !enum_sym || !info || !method || method->type != AST_METHOD_DECL)
        return;

    MethodDeclNode *md = &method->as.method_decl;
    XaSymbol *method_sym = xa_symbol_new(md->name, XA_SYM_METHOD);
    method_sym->location.line = method->line;
    method_sym->is_static = md->is_static;
    method_sym->is_private = md->is_private;
    method_sym->mutates_receiver = false;
    xa_visit_add_symbol_checked(ctx, method_sym, 0);

    XrType **param_types = NULL;
    const char **param_names = NULL;
    if (md->param_count > 0) {
        param_types = xr_malloc(sizeof(XrType *) * md->param_count);
        param_names = xr_malloc(sizeof(char *) * md->param_count);
        if (!param_types || !param_names) {
            xr_free(param_types);
            xr_free(param_names);
            param_types = NULL;
            param_names = NULL;
        }
        for (int i = 0; param_types && i < md->param_count; i++) {
            XrParamNode *param = md->params ? md->params[i] : NULL;
            param_types[i] = (param && param->type)
                                 ? xr_tref_resolve_in_analyzer(ctx->analyzer, param->type)
                                 : xr_type_new_unknown(NULL);
            param_names[i] = param ? param->name : NULL;
        }
    }

    XrType *ret_type =
        md->return_type ? xr_tref_resolve_in_analyzer(ctx->analyzer, md->return_type) : NULL;
    if (!ret_type)
        ret_type = xr_type_new_unit(NULL);

    XrType *method_type = xr_type_new_function(ctx->analyzer->isolate, param_types, md->param_count,
                                               ret_type, md->is_variadic);
    if (method_type)
        method_type->function.min_params = md->required_count;
    if (method_type && md->params) {
        for (int i = 0; i < md->param_count; i++) {
            XrParamNode *param = md->params[i];
            xr_type_function_set_param_mode(method_type, i,
                                            param ? param->passing_mode : XR_PARAM_VALUE);
        }
    }

    XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
    method_links->type = method_type;
    method_links->file_path = ctx->file_path;
    xa_symbol_links_set_function_sig(method_links, param_types, param_names, md->param_count,
                                     ret_type);
    if (md->param_count > 0) {
        AstNode **defs = (AstNode **) xr_calloc(md->param_count, sizeof(AstNode *));
        if (defs) {
            for (int i = 0; i < md->param_count; i++)
                defs[i] = md->params && md->params[i] ? md->params[i]->default_value : NULL;
            xa_bind_param_default_exprs(ctx, defs, param_types, md->param_count);
            xa_symbol_links_set_param_defaults(method_links, defs, md->param_count);
            xr_free(defs);
        }
    }

    xa_class_info_add_method(info, method_sym);

    if (md->body) {
        xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, method);

        if (!md->is_static) {
            XaSymbol *this_sym = xa_symbol_new("this", XA_SYM_PARAMETER);
            this_sym->location.line = method->line;
            xa_visit_add_symbol_checked(ctx, this_sym, 0);
            XaSymbolLinks *this_links = xa_analyzer_get_links(ctx->analyzer, this_sym);
            if (this_links) {
                this_links->type = enum_method_this_type(ctx, enum_sym->name);
                this_links->is_definitely_assigned = true;
            }
        }

        for (int i = 0; i < md->param_count; i++) {
            XrParamNode *source_param = md->params ? md->params[i] : NULL;
            const char *pname = source_param ? source_param->name : NULL;
            if (!pname)
                continue;
            XaSymbol *param = xa_visit_bind_parameter_symbol(ctx, source_param, method->line);
            if (!param)
                continue;
            XaSymbolLinks *plinks = xa_analyzer_get_links(ctx->analyzer, param);
            if (plinks) {
                XrType *param_type =
                    (method_links && method_links->param_types && i < method_links->param_count)
                        ? method_links->param_types[i]
                        : xr_type_new_unknown(NULL);
                if (md->is_variadic && i == md->param_count - 1)
                    param_type = xr_type_new_array(ctx->analyzer->isolate, param_type);
                plinks->type = param_type;
                plinks->is_definitely_assigned = param->passing_mode != XR_PARAM_OUT;
            }
        }

        xa_visit_collect(ctx, md->body);
        xa_analyzer_exit_scope(ctx->analyzer);
    }

    xr_free(param_types);
    xr_free(param_names);
}

void xa_visit_collect(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    switch (node->type) {
        case AST_PROGRAM:
            xa_visit_collect_program(ctx, node);
            break;
        case AST_FUNCTION_DECL:
            // When called directly (not from hoisting), do both phases
            xa_visit_collect_function(ctx, node);
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            xa_visit_collect_class(ctx, node);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            xa_visit_collect_var_decl(ctx, node);
            break;
        case AST_IMPORT_STMT:
            xa_visit_collect_import(ctx, node);
            break;
        case AST_GLOBAL_ASM:
            break;
        case AST_ENUM_DECL: {
            EnumDeclNode *edecl = &node->as.enum_decl;
            if (edecl->name) {
                XaSymbol *sym =
                    edecl->symbol_id
                        ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, edecl->symbol_id)
                        : NULL;
                if (!sym)
                    sym = xa_scope_lookup_local(ctx->analyzer->current_scope, edecl->name);
                if (!sym) {
                    sym = xa_symbol_new(edecl->name, XA_SYM_ENUM);
                    sym->location.line = node->line;
                    sym->is_const = true;
                    xa_visit_add_symbol_checked(ctx, sym, 0);
                    edecl->symbol_id = sym->id;
                }
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (!links->type)
                    links->type = xr_type_new_enum(ctx->analyzer->isolate, edecl->name);
                links->declared_type = links->type;
                XrClassInfo *enum_info =
                    links->class_info ? links->class_info : xa_class_info_new(edecl->name);
                links->class_info = enum_info;
                if (edecl->type_param_count > 0 && edecl->type_params) {
                    const char **type_param_names =
                        xr_malloc(sizeof(const char *) * (size_t) edecl->type_param_count);
                    if (type_param_names) {
                        for (int i = 0; i < edecl->type_param_count; i++) {
                            type_param_names[i] =
                                edecl->type_params[i] ? edecl->type_params[i]->name : NULL;
                        }
                        xa_symbol_links_set_type_params(links, type_param_names, NULL, NULL,
                                                        edecl->type_param_count);
                        xr_free(type_param_names);
                    }
                }
                XaScope *enum_payload_scope = ctx->analyzer->current_scope;
                XaSymbol *saved_enum_class_symbol =
                    enum_payload_scope ? enum_payload_scope->class_symbol : NULL;
                if (enum_payload_scope)
                    enum_payload_scope->class_symbol = sym;

                // Detect ADT enum: any variant with payload_count > 0
                bool is_adt = false;
                for (int m = 0; m < edecl->member_count && !is_adt; m++) {
                    AstNode *mem = edecl->members[m];
                    if (mem && mem->type == AST_ENUM_MEMBER &&
                        mem->as.enum_member.payload_count > 0) {
                        is_adt = true;
                    }
                }
                if (links->enum_info) {
                    xa_enum_info_free(links->enum_info);
                    links->enum_info = NULL;
                }

                // Store enum member names and payload metadata for exhaustiveness checking.
                if (edecl->member_count > 0) {
                    XaEnumInfo *enum_meta =
                        xa_enum_info_new(edecl->name, (uint32_t) edecl->member_count);
                    uint32_t enum_index = 0;
                    for (int m = 0; m < edecl->member_count; m++) {
                        AstNode *mem = edecl->members[m];
                        if (!mem || mem->type != AST_ENUM_MEMBER || !mem->as.enum_member.name)
                            continue;
                        if (!enum_meta || enum_index >= enum_meta->variant_count)
                            continue;
                        XaEnumVariantInfo *variant = &enum_meta->variants[enum_index];
                        variant->name = mem->as.enum_member.name;
                        int pc = mem->as.enum_member.payload_count;
                        variant->payload_count = (uint16_t) pc;
                        if (pc > 0 && mem->as.enum_member.payload_types) {
                            XrType **ptypes = xr_calloc((size_t) pc, sizeof(XrType *));
                            for (int p = 0; p < pc; p++) {
                                XrTypeRef *tref = mem->as.enum_member.payload_types[p];
                                XrType *ptype =
                                    tref ? xr_tref_resolve_in_analyzer(ctx->analyzer, tref)
                                         : xr_type_new_unknown(NULL);
                                if (ptype && edecl->type_param_count > 0 && edecl->type_params &&
                                    links->type_param_names) {
                                    ptype = resolve_class_to_type_param(
                                        ctx->analyzer->isolate, ptype, links->type_param_names,
                                        links->type_param_count);
                                }
                                if (ptype &&
                                    xa_enum_payload_contains_by_value(ptype, edecl->name)) {
                                    XrLocation loc = {.file = ctx->file_path,
                                                      .line = mem->line,
                                                      .column = mem->column};
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "enum '%s' variant '%s' cannot contain '%s' by value; "
                                             "use explicit indirection such as Box<%s> or a class "
                                             "node for recursive data",
                                             edecl->name ? edecl->name : "?",
                                             mem->as.enum_member.name ? mem->as.enum_member.name
                                                                      : "?",
                                             edecl->name ? edecl->name : "?",
                                             edecl->name ? edecl->name : "?");
                                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                                                               &loc);
                                }
                                ptypes[p] = ptype;
                            }
                            variant->payload_types = ptypes;
                        }
                        enum_index++;
                    }
                    if (enum_meta && enum_index == enum_meta->variant_count &&
                        xa_enum_info_finalize_layout(enum_meta)) {
                        links->enum_info = enum_meta;
                        if (links->type && links->type->kind == XR_KIND_ENUM) {
                            links->type->enum_type.layout = enum_meta->layout;
                            links->type->enum_type.layout_id =
                                enum_meta->layout ? enum_meta->layout->layout_id : 0;
                        }
                        if (links->declared_type && links->declared_type->kind == XR_KIND_ENUM) {
                            links->declared_type->enum_type.layout = enum_meta->layout;
                            links->declared_type->enum_type.layout_id =
                                enum_meta->layout ? enum_meta->layout->layout_id : 0;
                        }
                    } else if (enum_meta) {
                        xa_enum_info_free(enum_meta);
                    }
                }
                if (enum_payload_scope)
                    enum_payload_scope->class_symbol = saved_enum_class_symbol;

                if (enum_info && edecl->method_count > 0) {
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
                    ctx->analyzer->current_scope->class_symbol = sym;
                    for (int m = 0; m < edecl->method_count; m++) {
                        xa_visit_collect_enum_method(ctx, sym, enum_info, edecl->methods[m]);
                    }
                    xa_analyzer_exit_scope(ctx->analyzer);
                }
            }
            break;
        }
        case AST_TYPE_ALIAS: {
            TypeAliasNode *ta = &node->as.type_alias;
            if (ta->name) {
                XaSymbol *sym = xa_scope_lookup_local(ctx->analyzer->current_scope, ta->name);
                if (!sym || sym->kind != XA_SYM_TYPE_ALIAS) {
                    sym = xa_symbol_new(ta->name, XA_SYM_TYPE_ALIAS);
                    if (!sym)
                        break;
                    sym->location.line = node->line;
                    sym->location.column = node->column;
                    sym->is_const = true;
                    xa_visit_add_symbol_checked(ctx, sym, 0);
                }
                ta->symbol_id = sym->id;
                sym->type_alias_node = node;
                // Parser stashes the resolved type in
                // TypeAliasNode::resolved_type. Mirror it into the side
                // table so downstream readers (codegen, LSP) get the
                // same answer through the canonical lookup path.
                XrType *resolved =
                    ta->resolved_type
                        ? xr_tref_resolve_in_analyzer(ctx->analyzer, ta->resolved_type)
                        : NULL;
                if (resolved) {
                    xa_analyzer_set_node_type(ctx->analyzer, node, resolved);
                }
                sym->alias_type = resolved;

                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links) {
                    links->type = resolved ? resolved : xr_type_new_unknown(NULL);
                    links->declared_type = links->type;
                    if (!links->file_path)
                        links->file_path = ctx->file_path;
                }
            }
            break;
        }
        case AST_FOR_IN_STMT: {
            // Register for-in iteration variable in a block scope
            ForInStmtNode *fi = &node->as.for_in_stmt;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
            if (fi->item_name) {
                XaSymbol *sym = xa_symbol_new(fi->item_name, XA_SYM_VARIABLE);
                sym->location.line = node->line;
                sym->is_const = true;  // for-in loop variable is immutable
                xa_visit_add_symbol_checked(ctx, sym, 0);
                fi->item_symbol_id = sym->id;
                XaSymbolLinks *item_links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (item_links)
                    item_links->is_definitely_assigned = true;
            }
            if (fi->is_keyvalue && fi->value_name) {
                XaSymbol *vsym = xa_symbol_new(fi->value_name, XA_SYM_VARIABLE);
                vsym->location.line = node->line;
                vsym->is_const = true;  // for-in loop variable is immutable
                xa_visit_add_symbol_checked(ctx, vsym, 0);
                fi->value_symbol_id = vsym->id;
                XaSymbolLinks *val_links = xa_analyzer_get_links(ctx->analyzer, vsym);
                if (val_links)
                    val_links->is_definitely_assigned = true;
            }
            if (fi->body) {
                xa_visit_collect(ctx, fi->body);
            }
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_BLOCK:
            /* Bare block statements (standalone { ... }) get a scope from
             * xa_visit_collect_statements_with_hoisting which wraps them.
             * Structural blocks (while/for/if body) call here directly
             * and must NOT get an extra scope — their enclosing construct
             * already manages scoping in Pass 2. */
            xa_visit_collect_statements_with_hoisting(ctx, node->as.block.statements,
                                                      node->as.block.count);
            break;

        // Recurse into control flow statements to collect nested declarations
        case AST_FOR_STMT: {
            ForStmtNode *fs = &node->as.for_stmt;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
            if (fs->initializer)
                xa_visit_collect(ctx, fs->initializer);
            if (fs->body)
                xa_visit_collect(ctx, fs->body);
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_WHILE_STMT:
            /* The body is its own block scope in Pass 2 (via
             * xa_visit_block_stmt). Pass 1 must mirror that so symbols
             * declared in one loop body don't collide with same-named
             * symbols in sibling loops. */
            if (node->as.while_stmt.body) {
                bool is_block = node->as.while_stmt.body->type == AST_BLOCK;
                if (is_block)
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK,
                                            node->as.while_stmt.body);
                xa_visit_collect(ctx, node->as.while_stmt.body);
                if (is_block)
                    xa_analyzer_exit_scope(ctx->analyzer);
            }
            break;
        case AST_IF_STMT:
            /* Each branch is its own block scope in Pass 2 (via
             * xa_visit_block_stmt). Pass 1 must mirror that so symbols
             * declared in then-branch don't collide with same-named
             * symbols in else-branch. */
            if (node->as.if_stmt.then_branch) {
                bool is_block = node->as.if_stmt.then_branch->type == AST_BLOCK;
                if (is_block)
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK,
                                            node->as.if_stmt.then_branch);
                xa_visit_collect(ctx, node->as.if_stmt.then_branch);
                if (is_block)
                    xa_analyzer_exit_scope(ctx->analyzer);
            }
            if (node->as.if_stmt.else_branch) {
                bool is_block = node->as.if_stmt.else_branch->type == AST_BLOCK;
                if (is_block)
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK,
                                            node->as.if_stmt.else_branch);
                xa_visit_collect(ctx, node->as.if_stmt.else_branch);
                if (is_block)
                    xa_analyzer_exit_scope(ctx->analyzer);
            }
            break;
        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            if (tc->try_body) {
                bool is_block = tc->try_body->type == AST_BLOCK;
                if (is_block)
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, tc->try_body);
                xa_visit_collect(ctx, tc->try_body);
                if (is_block)
                    xa_analyzer_exit_scope(ctx->analyzer);
            }
            for (int ci = 0; ci < tc->catch_count; ci++) {
                XrCatchClause *cc = tc->catch_clauses[ci];
                if (!cc || !cc->body)
                    continue;
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, cc->body);
                if (cc->var_name) {
                    XaSymbol *err_sym = xa_symbol_new(cc->var_name, XA_SYM_VARIABLE);
                    err_sym->location.line = cc->var_line;
                    err_sym->location.column = cc->var_column;
                    xa_visit_add_symbol_checked(ctx, err_sym, 0);
                    cc->symbol_id = err_sym->id;
                    XaSymbolLinks *err_links = xa_analyzer_get_links(ctx->analyzer, err_sym);
                    if (err_links) {
                        err_links->type = xa_resolve_catch_binding_type(ctx, cc, false);
                        err_links->is_definitely_assigned = true;
                    }
                }
                if (cc->pattern) {
                    XrType *catch_type = xa_resolve_catch_binding_type(ctx, cc, false);
                    xa_register_catch_pattern_bindings(ctx, cc, catch_type);
                }
                xa_visit_collect(ctx, cc->body);
                xa_analyzer_exit_scope(ctx->analyzer);
            }
            break;
        }
        case AST_SELECT_STMT: {
            SelectStmtNode *sel = &node->as.select_stmt;
            for (int i = 0; i < sel->case_count; i++) {
                if (sel->cases[i])
                    xa_visit_collect(ctx, sel->cases[i]);
            }
            break;
        }
        case AST_SELECT_CASE: {
            SelectCaseNode *sc = &node->as.select_case;
            /* Recv cases introduce a variable: `msg from ch -> ...`
             * Enter a block scope for the case body so `msg` is visible. */
            if (sc->var_name && !sc->is_send && !sc->is_default) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
                XaSymbol *sym = xa_symbol_new(sc->var_name, XA_SYM_VARIABLE);
                sym->location.line = node->line;
                xa_visit_add_symbol_checked(ctx, sym, 0);
                sc->var_symbol_id = sym->id;
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links)
                    links->is_definitely_assigned = true;
                if (sc->body)
                    xa_visit_collect(ctx, sc->body);
                xa_analyzer_exit_scope(ctx->analyzer);
            } else {
                if (sc->body)
                    xa_visit_collect(ctx, sc->body);
            }
            break;
        }
        case AST_SCOPE_BLOCK:
            /* Pass 2's xa_visit_block_stmt enters a BLOCK scope keyed on the
             * body AST node. Pass 1 must create the matching scope so that
             * nested constructs (e.g. for-in) register their symbols under
             * the same parent chain. Otherwise the for-in scope from Pass 1
             * becomes a sibling of Pass 2's lookup root and its loop
             * variable lookup fails. */
            if (node->as.scope_block.body) {
                AstNode *body = node->as.scope_block.body;
                if (body->type == AST_BLOCK) {
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, body);
                    xa_visit_collect(ctx, body);
                    xa_analyzer_exit_scope(ctx->analyzer);
                } else {
                    xa_visit_collect(ctx, body);
                }
            }
            break;
        case AST_EXPR_STMT:
            if (node->as.expr_stmt)
                xa_visit_collect(ctx, node->as.expr_stmt);
            break;
        case AST_UNSAFE_EXPR:
            if (node->as.unsafe_expr.operand)
                xa_visit_collect(ctx, node->as.unsafe_expr.operand);
            break;

        // Collect destructuring and multi-var declarations
        case AST_DESTRUCTURE_DECL: {
            DestructureDeclNode *dd = &node->as.destructure_decl;
            if (dd->pattern) {
                XrDestructurePattern *pat = dd->pattern;
                if (pat->type == PATTERN_ARRAY || pat->type == PATTERN_TUPLE) {
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym =
                                xa_symbol_new(elem->as.identifier.name, XA_SYM_VARIABLE);
                            sym->is_const = dd->is_const;
                            sym->location.line = node->line;
                            xa_visit_add_symbol_checked(ctx, sym, 0);
                            elem->as.identifier.symbol_id = sym->id;
                        }
                    }
                } else if (pat->type == PATTERN_OBJECT) {
                    for (int i = 0; i < pat->as.object.field_count; i++) {
                        XrDestructurePattern *vp = pat->as.object.patterns[i];
                        if (vp && vp->type == PATTERN_IDENTIFIER && vp->as.identifier.name) {
                            XaSymbol *sym = xa_symbol_new(vp->as.identifier.name, XA_SYM_VARIABLE);
                            sym->is_const = dd->is_const;
                            sym->location.line = node->line;
                            xa_visit_add_symbol_checked(ctx, sym, 0);
                            vp->as.identifier.symbol_id = sym->id;
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void xa_visit_collect_program(XaInferContext *ctx, AstNode *node) {
    if (!node || node->type != AST_PROGRAM)
        return;

    ProgramNode *prog = &node->as.program;
    // Use hoisting to support mutual recursion at module level
    xa_visit_collect_statements_with_hoisting(ctx, prog->statements, prog->count);
}

/* ============================================================================
 * Pass 2: Type Inference - Entry Points
 * ============================================================================
 * xa_visit_infer()      - Main dispatch for expressions and statements
 * xa_visit_infer_expr() - Expression type inference
 * xa_visit_infer_stmt() - Statement type inference
 * ========================================================================== */

XrType *xa_visit_infer(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    // Check if it's an expression or statement
    switch (node->type) {
        // Expressions
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_STRING:
        case AST_FIXED_BYTES_LITERAL:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_VARIABLE:
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_CALL_EXPR:
        case AST_MEMBER_ACCESS:
        case AST_INDEX_GET:
        case AST_ARRAY_LITERAL:
        case AST_TUPLE_LITERAL:
        case AST_MAP_LITERAL:
        case AST_OBJECT_LITERAL:
        case AST_NEW_EXPR:
        case AST_TERNARY:
        case AST_FUNCTION_EXPR:
        case AST_COMPTIME_EXPR:
            return xa_visit_infer_expr(ctx, node);

        // Statements
        default:
            xa_visit_infer_stmt(ctx, node);
            return xr_type_new_unit(NULL);
    }
}

static XrType *xa_visit_comptime_expr(XaInferContext *ctx, AstNode *node) {
    AstNode *inner = node ? node->as.comptime_expr.expr : NULL;
    if (!ctx || !ctx->analyzer || !node || !inner)
        return xr_type_new_unknown(NULL);

    if (inner->type == AST_BLOCK)
        return xa_visit_comptime_block_expr(ctx, node);

    XrType *inner_type = xa_visit_infer_expr(ctx, inner);
    XrCtValue value = {0};
    const char *err = NULL;
    if (!xa_consteval_expr(ctx->analyzer, inner, &value, &err)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "comptime expression must be evaluable at compile time%s%s",
                 err ? ": " : "", err ? err : "");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
    } else {
        xa_analyzer_set_node_ct_value(ctx->analyzer, node, &value);
    }
    return inner_type ? inner_type : xr_type_new_unknown(NULL);
}

static bool xa_is_comptime_block_expr(const AstNode *node) {
    return node && node->type == AST_COMPTIME_EXPR && node->as.comptime_expr.expr &&
           node->as.comptime_expr.expr->type == AST_BLOCK;
}

static void xa_clear_ct_value_cache_expr(XaAnalyzer *analyzer, AstNode *expr) {
    if (!analyzer || !expr)
        return;

    xa_analyzer_set_node_ct_value(analyzer, expr, NULL);
    switch (expr->type) {
        case AST_COMPTIME_EXPR:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.comptime_expr.expr);
            break;
        case AST_GROUPING:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.grouping);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.unary.operand);
            break;
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
        case AST_NULLISH_COALESCE:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.binary.left);
            xa_clear_ct_value_cache_expr(analyzer, expr->as.binary.right);
            break;
        case AST_TERNARY:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.ternary.condition);
            xa_clear_ct_value_cache_expr(analyzer, expr->as.ternary.true_expr);
            xa_clear_ct_value_cache_expr(analyzer, expr->as.ternary.false_expr);
            break;
        case AST_ARRAY_LITERAL:
            if (expr->as.array_literal.is_repeat) {
                xa_clear_ct_value_cache_expr(analyzer, expr->as.array_literal.repeat_value);
                xa_clear_ct_value_cache_expr(analyzer, expr->as.array_literal.repeat_count);
            }
            for (int i = 0; i < expr->as.array_literal.count; i++)
                xa_clear_ct_value_cache_expr(analyzer, expr->as.array_literal.elements[i]);
            break;
        case AST_TUPLE_LITERAL:
            for (int i = 0; i < expr->as.tuple_literal.count; i++)
                xa_clear_ct_value_cache_expr(analyzer, expr->as.tuple_literal.elements[i]);
            break;
        case AST_SPREAD_EXPR:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.spread_expr.expr);
            break;
        case AST_STRUCT_LITERAL:
            for (int i = 0; i < expr->as.struct_literal.field_count; i++)
                xa_clear_ct_value_cache_expr(analyzer, expr->as.struct_literal.field_values[i]);
            break;
        case AST_MEMBER_ACCESS:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.member_access.object);
            break;
        case AST_INDEX_GET:
            xa_clear_ct_value_cache_expr(analyzer, expr->as.index_get.array);
            xa_clear_ct_value_cache_expr(analyzer, expr->as.index_get.index);
            break;
        default:
            break;
    }
}

static void xa_report_comptime_block_error(XaInferContext *ctx, AstNode *loc_node,
                                           const char *message) {
    if (!ctx || !ctx->analyzer || !message)
        return;
    XrLocation loc = {.file = ctx->file_path,
                      .line = loc_node ? loc_node->line : 0,
                      .column = loc_node ? loc_node->column : 0};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                               message, &loc);
}

static bool xa_eval_comptime_block_arg(XaInferContext *ctx, AstNode *arg, XrCtValue *out,
                                       const char *subject) {
    if (!ctx || !arg || !out)
        return false;
    xa_clear_ct_value_cache_expr(ctx->analyzer, arg);
    xa_visit_infer_expr(ctx, arg);
    const char *err = NULL;
    if (xa_consteval_expr(ctx->analyzer, arg, out, &err))
        return true;

    char msg[256];
    snprintf(msg, sizeof(msg), "%s must be evaluable at compile time%s%s", subject, err ? ": " : "",
             err ? err : "");
    xa_report_comptime_block_error(ctx, arg, msg);
    return false;
}

static const char *xa_comptime_block_supported_message(void) {
    return "comptime block currently supports const/var declarations, local var assignments, "
           "local var compound assignments, compile-time if/while statements, "
           "C-style for loops, fixed-array for-in loops, break/continue inside comptime loops, "
           "compile_assert(...) and compile_error(...)";
}

typedef enum XaComptimeBlockFlowKind {
    XA_COMPTIME_FLOW_NORMAL = 0,
    XA_COMPTIME_FLOW_BREAK,
    XA_COMPTIME_FLOW_CONTINUE,
    XA_COMPTIME_FLOW_RETURN,
} XaComptimeBlockFlowKind;

typedef struct XaComptimeBlockFlow {
    XaComptimeBlockFlowKind kind;
    const char *label;
    AstNode *node;
} XaComptimeBlockFlow;

static XaComptimeBlockFlow xa_comptime_block_flow(XaComptimeBlockFlowKind kind, const char *label,
                                                  AstNode *node) {
    XaComptimeBlockFlow flow = {.kind = kind, .label = label, .node = node};
    return flow;
}

static XaComptimeBlockFlow xa_comptime_block_flow_normal(void) {
    return xa_comptime_block_flow(XA_COMPTIME_FLOW_NORMAL, NULL, NULL);
}

static bool xa_comptime_block_flow_targets_loop(const XaComptimeBlockFlow *flow,
                                                const char *loop_label) {
    if (!flow)
        return false;
    if (flow->kind != XA_COMPTIME_FLOW_BREAK && flow->kind != XA_COMPTIME_FLOW_CONTINUE)
        return false;
    if (!flow->label)
        return true;
    return loop_label && strcmp(flow->label, loop_label) == 0;
}

static void xa_report_unhandled_comptime_loop_control(XaInferContext *ctx,
                                                      const XaComptimeBlockFlow *flow,
                                                      AstNode *fallback) {
    if (!ctx || !flow)
        return;
    AstNode *node = flow->node ? flow->node : fallback;
    if (flow->kind == XA_COMPTIME_FLOW_BREAK) {
        if (flow->label) {
            char msg[160];
            snprintf(msg, sizeof(msg), "unknown loop label '%s' for 'break'", flow->label);
            xa_report_comptime_block_error(ctx, node, msg);
        } else {
            xa_report_comptime_block_error(ctx, node,
                                           "comptime break can only be used inside comptime loop");
        }
        return;
    }
    if (flow->kind == XA_COMPTIME_FLOW_CONTINUE) {
        if (flow->label) {
            char msg[160];
            snprintf(msg, sizeof(msg), "unknown loop label '%s' for 'continue'", flow->label);
            xa_report_comptime_block_error(ctx, node, msg);
        } else {
            xa_report_comptime_block_error(
                ctx, node, "comptime continue can only be used inside comptime loop");
        }
    }
}

typedef struct XaComptimeBlockReturn {
    bool allow_return;
    bool has_value;
    XrCtValue value;
    XrType *type;
} XaComptimeBlockReturn;

static XaComptimeBlockFlow xa_visit_comptime_block_statement(XaInferContext *ctx, AstNode *stmt,
                                                             XaComptimeBlockReturn *ret);

static bool xa_comptime_compound_op_to_binary(XrTokenType op, AstNodeType *out) {
    if (!out)
        return false;
    switch (op) {
        case TK_PLUS_ASSIGN:
            *out = AST_BINARY_ADD;
            return true;
        case TK_MINUS_ASSIGN:
            *out = AST_BINARY_SUB;
            return true;
        case TK_MUL_ASSIGN:
            *out = AST_BINARY_MUL;
            return true;
        case TK_DIV_ASSIGN:
            *out = AST_BINARY_DIV;
            return true;
        case TK_MOD_ASSIGN:
            *out = AST_BINARY_MOD;
            return true;
        case TK_AND_ASSIGN:
            *out = AST_BINARY_BAND;
            return true;
        case TK_OR_ASSIGN:
            *out = AST_BINARY_BOR;
            return true;
        case TK_XOR_ASSIGN:
            *out = AST_BINARY_BXOR;
            return true;
        case TK_LSHIFT_ASSIGN:
            *out = AST_BINARY_LSHIFT;
            return true;
        case TK_RSHIFT_ASSIGN:
            *out = AST_BINARY_RSHIFT;
            return true;
        default:
            return false;
    }
}

static void xa_visit_comptime_block_assignment(XaInferContext *ctx, AstNode *stmt,
                                               AssignmentNode *assign) {
    if (!ctx || !ctx->analyzer || !stmt || !assign || !assign->name)
        return;

    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, assign->name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
    if (!sym || !links || !links->is_comptime_local || sym->is_const || sym->is_shared ||
        !sym->is_rebindable) {
        xa_report_comptime_block_error(
            ctx, stmt, "comptime block assignment target must be a block-local var");
        return;
    }

    assign->symbol_id = sym->id;
    xa_clear_ct_value_cache_expr(ctx->analyzer, assign->value);
    XrType *value_type = xa_visit_infer_expr(ctx, assign->value);
    XrType *target_type = links->type ? links->type : xa_analyzer_get_type(ctx->analyzer, sym);
    XrLocation loc = {.file = ctx->file_path, .line = stmt->line, .column = stmt->column};
    if (target_type && value_type &&
        !xa_analyzer_check_assignment(ctx->analyzer, target_type, value_type, &loc))
        return;

    XrCtValue value = {0};
    const char *err = NULL;
    if (!xa_consteval_expr(ctx->analyzer, assign->value, &value, &err)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "comptime block assignment must be evaluable at compile time%s%s", err ? ": " : "",
                 err ? err : "");
        xa_report_comptime_block_error(ctx, stmt, msg);
        return;
    }

    links->has_ct_value = true;
    links->ct_value = value;
    links->is_const_foldable = true;
    links->assign_count++;
}

static void xa_visit_comptime_block_compound_assignment(XaInferContext *ctx, AstNode *stmt,
                                                        CompoundAssignmentNode *compound) {
    if (!ctx || !ctx->analyzer || !stmt || !compound || !compound->name)
        return;
    if (compound->object) {
        xa_report_comptime_block_error(
            ctx, stmt, "comptime block compound assignment target must be a block-local var");
        return;
    }

    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, compound->name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
    if (!sym || !links || !links->is_comptime_local || sym->is_const || sym->is_shared ||
        !sym->is_rebindable) {
        xa_report_comptime_block_error(
            ctx, stmt, "comptime block compound assignment target must be a block-local var");
        return;
    }

    AstNodeType binary_type = AST_BINARY_ADD;
    if (!xa_comptime_compound_op_to_binary(compound->op, &binary_type)) {
        xa_report_comptime_block_error(ctx, stmt,
                                       "unsupported comptime block compound assignment operator");
        return;
    }

    XrCompilerSession *session = ctx->analyzer->compiler_session;
    if (!session) {
        xa_report_comptime_block_error(ctx, stmt,
                                       "comptime block compound assignment requires a compiler "
                                       "session");
        return;
    }

    compound->symbol_id = sym->id;
    AstNode lhs = {0};
    lhs.type = AST_VARIABLE;
    lhs.node_id = xr_compiler_session_next_ast_node_id(session);
    lhs.line = stmt->line;
    lhs.column = stmt->column;
    lhs.as.variable.name = compound->name;
    lhs.as.variable.symbol_id = sym->id;

    AstNode binary = {0};
    binary.type = binary_type;
    binary.node_id = xr_compiler_session_next_ast_node_id(session);
    binary.line = stmt->line;
    binary.column = stmt->column;
    binary.as.binary.left = &lhs;
    binary.as.binary.right = compound->value;

    xa_clear_ct_value_cache_expr(ctx->analyzer, compound->value);
    XrType *value_type = xa_visit_infer_expr(ctx, &binary);
    XrType *target_type = links->type ? links->type : xa_analyzer_get_type(ctx->analyzer, sym);
    XrLocation loc = {.file = ctx->file_path, .line = stmt->line, .column = stmt->column};
    if (target_type && value_type &&
        !xa_analyzer_check_assignment(ctx->analyzer, target_type, value_type, &loc))
        return;

    XrCtValue value = {0};
    const char *err = NULL;
    if (!xa_consteval_expr(ctx->analyzer, &binary, &value, &err)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "comptime block compound assignment must be evaluable at compile time%s%s",
                 err ? ": " : "", err ? err : "");
        xa_report_comptime_block_error(ctx, stmt, msg);
        return;
    }

    links->has_ct_value = true;
    links->ct_value = value;
    links->is_const_foldable = true;
    links->assign_count++;
}

static void xa_visit_comptime_compile_assert(XaInferContext *ctx, AstNode *stmt,
                                             CallExprNode *call) {
    if (!ctx || !stmt || !call)
        return;
    if (call->arg_count < 1 || call->arg_count > 2) {
        xa_report_comptime_block_error(ctx, stmt, "compile_assert expects 1 or 2 arguments");
        return;
    }

    XrCtValue condition = {0};
    if (!xa_eval_comptime_block_arg(ctx, call->arguments[0], &condition,
                                    "compile_assert condition"))
        return;
    if (condition.kind != XR_CT_BOOL) {
        xa_report_comptime_block_error(ctx, call->arguments[0],
                                       "compile_assert condition must be a compile-time bool");
        return;
    }

    const char *message = NULL;
    if (call->arg_count == 2) {
        XrCtValue msg_value = {0};
        if (!xa_eval_comptime_block_arg(ctx, call->arguments[1], &msg_value,
                                        "compile_assert message"))
            return;
        if (msg_value.kind != XR_CT_STRING) {
            xa_report_comptime_block_error(ctx, call->arguments[1],
                                           "compile_assert message must be a compile-time string");
            return;
        }
        message = msg_value.as.string_val;
    }

    if (!condition.as.bool_val) {
        char msg[512];
        if (message && message[0])
            snprintf(msg, sizeof(msg), "compile_assert failed: %s", message);
        else
            snprintf(msg, sizeof(msg), "compile_assert failed");
        xa_report_comptime_block_error(ctx, stmt, msg);
    }
}

static void xa_visit_comptime_compile_error(XaInferContext *ctx, AstNode *stmt,
                                            CallExprNode *call) {
    if (!ctx || !stmt || !call)
        return;
    if (call->arg_count != 1) {
        xa_report_comptime_block_error(ctx, stmt, "compile_error expects 1 argument");
        return;
    }

    XrCtValue message = {0};
    if (!xa_eval_comptime_block_arg(ctx, call->arguments[0], &message, "compile_error message"))
        return;
    if (message.kind != XR_CT_STRING) {
        xa_report_comptime_block_error(ctx, call->arguments[0],
                                       "compile_error message must be a compile-time string");
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "compile_error: %s",
             message.as.string_val ? message.as.string_val : "");
    xa_report_comptime_block_error(ctx, stmt, msg);
}

static XaComptimeBlockFlow xa_visit_comptime_block_nested_block(XaInferContext *ctx,
                                                                AstNode *block_node,
                                                                XaComptimeBlockReturn *ret) {
    if (!ctx || !block_node)
        return xa_comptime_block_flow_normal();
    if (block_node->type != AST_BLOCK) {
        return xa_visit_comptime_block_statement(ctx, block_node, ret);
    }

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, block_node);
    BlockNode *block = &block_node->as.block;
    XaComptimeBlockFlow flow = xa_comptime_block_flow_normal();
    for (int i = 0; i < block->count; i++) {
        flow = xa_visit_comptime_block_statement(ctx, block->statements[i], ret);
        if (flow.kind != XA_COMPTIME_FLOW_NORMAL)
            break;
    }
    xa_analyzer_exit_scope(ctx->analyzer);
    return flow;
}

static XaComptimeBlockFlow xa_visit_comptime_block_if_stmt(XaInferContext *ctx, AstNode *stmt,
                                                           IfStmtNode *if_stmt,
                                                           XaComptimeBlockReturn *ret) {
    if (!ctx || !stmt || !if_stmt)
        return xa_comptime_block_flow_normal();

    XrCtValue condition = {0};
    if (!xa_eval_comptime_block_arg(ctx, if_stmt->condition, &condition, "comptime if condition"))
        return xa_comptime_block_flow_normal();
    if (condition.kind != XR_CT_BOOL) {
        xa_report_comptime_block_error(ctx, if_stmt->condition,
                                       "comptime if condition must be a compile-time bool");
        return xa_comptime_block_flow_normal();
    }

    AstNode *selected = condition.as.bool_val ? if_stmt->then_branch : if_stmt->else_branch;
    if (selected)
        return xa_visit_comptime_block_nested_block(ctx, selected, ret);
    return xa_comptime_block_flow_normal();
}

#define XA_COMPTIME_WHILE_MAX_ITERATIONS 100000

static XaComptimeBlockFlow xa_visit_comptime_block_while_stmt(XaInferContext *ctx, AstNode *stmt,
                                                              WhileStmtNode *while_stmt,
                                                              XaComptimeBlockReturn *ret) {
    if (!ctx || !stmt || !while_stmt)
        return xa_comptime_block_flow_normal();

    XaLoopScope loop_scope;
    xa_loop_scope_push(ctx, &loop_scope, while_stmt->label, stmt);
    XaComptimeBlockFlow result = xa_comptime_block_flow_normal();
    for (int iteration = 0; iteration < XA_COMPTIME_WHILE_MAX_ITERATIONS; iteration++) {
        XrCtValue condition = {0};
        if (!xa_eval_comptime_block_arg(ctx, while_stmt->condition, &condition,
                                        "comptime while condition"))
            goto done;
        if (condition.kind != XR_CT_BOOL) {
            xa_report_comptime_block_error(ctx, while_stmt->condition,
                                           "comptime while condition must be a compile-time bool");
            goto done;
        }
        if (!condition.as.bool_val)
            goto done;

        int body_diag_count = ctx->analyzer ? ctx->analyzer->diagnostic_count : 0;
        XaComptimeBlockFlow flow = xa_comptime_block_flow_normal();
        if (while_stmt->body)
            flow = xa_visit_comptime_block_nested_block(ctx, while_stmt->body, ret);
        if (ctx->analyzer && ctx->analyzer->diagnostic_count > body_diag_count)
            goto done;
        if (flow.kind == XA_COMPTIME_FLOW_BREAK) {
            if (xa_comptime_block_flow_targets_loop(&flow, while_stmt->label))
                goto done;
            result = flow;
            goto done;
        }
        if (flow.kind == XA_COMPTIME_FLOW_CONTINUE) {
            if (xa_comptime_block_flow_targets_loop(&flow, while_stmt->label))
                continue;
            result = flow;
            goto done;
        }
        if (flow.kind == XA_COMPTIME_FLOW_RETURN) {
            result = flow;
            goto done;
        }
    }

    xa_report_comptime_block_error(ctx, stmt, "comptime while loop exceeded iteration limit");
done:
    xa_loop_scope_pop(ctx, &loop_scope);
    return result;
}

static XaComptimeBlockFlow xa_visit_comptime_block_for_stmt(XaInferContext *ctx, AstNode *stmt,
                                                            ForStmtNode *for_stmt,
                                                            XaComptimeBlockReturn *ret) {
    if (!ctx || !stmt || !for_stmt)
        return xa_comptime_block_flow_normal();

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, stmt);
    XaComptimeBlockFlow result = xa_comptime_block_flow_normal();
    XaLoopScope loop_scope;
    bool loop_scope_pushed = false;

    if (for_stmt->initializer) {
        int init_diag_count = ctx->analyzer ? ctx->analyzer->diagnostic_count : 0;
        result = xa_visit_comptime_block_statement(ctx, for_stmt->initializer, ret);
        if (ctx->analyzer && ctx->analyzer->diagnostic_count > init_diag_count)
            goto done;
        if (result.kind != XA_COMPTIME_FLOW_NORMAL) {
            xa_report_comptime_block_error(ctx, for_stmt->initializer,
                                           "comptime for initializer cannot use loop control");
            result = xa_comptime_block_flow_normal();
            goto done;
        }
    }

    xa_loop_scope_push(ctx, &loop_scope, for_stmt->label, stmt);
    loop_scope_pushed = true;
    for (int iteration = 0; iteration < XA_COMPTIME_WHILE_MAX_ITERATIONS; iteration++) {
        if (for_stmt->condition) {
            XrCtValue condition = {0};
            if (!xa_eval_comptime_block_arg(ctx, for_stmt->condition, &condition,
                                            "comptime for condition"))
                goto done;
            if (condition.kind != XR_CT_BOOL) {
                xa_report_comptime_block_error(
                    ctx, for_stmt->condition, "comptime for condition must be a compile-time bool");
                goto done;
            }
            if (!condition.as.bool_val)
                goto done;
        }

        int body_diag_count = ctx->analyzer ? ctx->analyzer->diagnostic_count : 0;
        XaComptimeBlockFlow flow = xa_comptime_block_flow_normal();
        if (for_stmt->body)
            flow = xa_visit_comptime_block_nested_block(ctx, for_stmt->body, ret);
        if (ctx->analyzer && ctx->analyzer->diagnostic_count > body_diag_count)
            goto done;
        if (flow.kind == XA_COMPTIME_FLOW_BREAK) {
            if (xa_comptime_block_flow_targets_loop(&flow, for_stmt->label))
                goto done;
            result = flow;
            goto done;
        }
        if (flow.kind == XA_COMPTIME_FLOW_RETURN) {
            result = flow;
            goto done;
        }
        if (flow.kind == XA_COMPTIME_FLOW_CONTINUE &&
            !xa_comptime_block_flow_targets_loop(&flow, for_stmt->label)) {
            result = flow;
            goto done;
        }

        if (for_stmt->increment) {
            int inc_diag_count = ctx->analyzer ? ctx->analyzer->diagnostic_count : 0;
            XaComptimeBlockFlow inc_flow =
                xa_visit_comptime_block_statement(ctx, for_stmt->increment, ret);
            if (ctx->analyzer && ctx->analyzer->diagnostic_count > inc_diag_count)
                goto done;
            if (inc_flow.kind != XA_COMPTIME_FLOW_NORMAL) {
                xa_report_comptime_block_error(ctx, for_stmt->increment,
                                               "comptime for increment cannot use loop control");
                goto done;
            }
        }
        if (flow.kind == XA_COMPTIME_FLOW_CONTINUE)
            continue;
    }

    xa_report_comptime_block_error(ctx, stmt, "comptime for loop exceeded iteration limit");

done:
    if (loop_scope_pushed)
        xa_loop_scope_pop(ctx, &loop_scope);
    xa_analyzer_exit_scope(ctx->analyzer);
    return result;
}

static XrType *xa_type_from_ct_value(XaInferContext *ctx, const XrCtValue *value) {
    XrVMRuntime *X = (ctx && ctx->analyzer) ? ctx->analyzer->isolate : NULL;
    if (!value)
        return xr_type_new_unknown(X);
    switch (value->kind) {
        case XR_CT_INT:
            return xr_type_new_int(X);
        case XR_CT_FLOAT:
            return xr_type_new_float(X);
        case XR_CT_BOOL:
            return xr_type_new_bool(X);
        case XR_CT_STRING:
            return xr_type_new_string(X);
        case XR_CT_CHAR:
            return xr_type_new_rune(X);
        case XR_CT_NULL:
            return xr_type_new_null(X);
        default:
            return xr_type_new_unknown(X);
    }
}

static XaSymbol *xa_ensure_comptime_for_in_symbol(XaInferContext *ctx, AstNode *stmt,
                                                  const char *name, uint32_t *symbol_id,
                                                  XrType *type) {
    if (!ctx || !ctx->analyzer || !name)
        return NULL;

    XaSymbol *sym = NULL;
    if (symbol_id && *symbol_id != 0)
        sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, *symbol_id);
    if (!sym)
        sym = xa_scope_lookup_local(ctx->analyzer->current_scope, name);
    if (!sym) {
        sym = xa_symbol_new(name, XA_SYM_VARIABLE);
        sym->location.line = stmt ? stmt->line : 0;
        sym->is_const = true;
        sym->is_readonly_binding = true;
        sym->is_rebindable = false;
        xa_visit_add_symbol_checked(ctx, sym, 0);
    }
    if (symbol_id && *symbol_id == 0)
        *symbol_id = sym->id;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        links->type = type ? type : xr_type_new_unknown(ctx->analyzer->isolate);
        links->declared_type = links->type;
        links->is_definitely_assigned = true;
        links->is_comptime_local = true;
        links->is_const_foldable = true;
    }
    return sym;
}

static void xa_set_comptime_for_in_symbol_value(XaInferContext *ctx, XaSymbol *sym,
                                                const XrCtValue *value, XrType *type) {
    if (!ctx || !ctx->analyzer || !sym || !value)
        return;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links)
        return;
    links->type = type ? type : xa_type_from_ct_value(ctx, value);
    links->declared_type = links->type;
    links->has_ct_value = true;
    links->ct_value = *value;
    links->is_const_foldable = true;
    links->is_definitely_assigned = true;
}

static XaComptimeBlockFlow xa_visit_comptime_block_for_in_stmt(XaInferContext *ctx, AstNode *stmt,
                                                               ForInStmtNode *fi,
                                                               XaComptimeBlockReturn *ret) {
    if (!ctx || !ctx->analyzer || !stmt || !fi)
        return xa_comptime_block_flow_normal();

    XrType *collection_type = NULL;
    if (fi->collection) {
        xa_clear_ct_value_cache_expr(ctx->analyzer, fi->collection);
        collection_type = xa_visit_infer_expr(ctx, fi->collection);
    }

    XrCtValue collection = {0};
    const char *err = NULL;
    if (!fi->collection || !xa_consteval_expr(ctx->analyzer, fi->collection, &collection, &err)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "comptime for-in collection must be evaluable at compile time%s%s",
                 err ? ": " : "", err ? err : "");
        xa_report_comptime_block_error(ctx, fi->collection ? fi->collection : stmt, msg);
        return xa_comptime_block_flow_normal();
    }

    if (collection.kind == XR_CT_TUPLE) {
        xa_report_comptime_block_error(ctx, fi->collection,
                                       "tuple values are not iterable in comptime for-in; use "
                                       "'.0', '.1' or destructuring instead");
        return xa_comptime_block_flow_normal();
    }
    if (collection.kind != XR_CT_FIXED_ARRAY) {
        xa_report_comptime_block_error(
            ctx, fi->collection,
            "comptime for-in currently supports fixed-array compile-time values");
        return xa_comptime_block_flow_normal();
    }

    XrType *element_type = NULL;
    if (collection_type) {
        if (collection_type->kind == XR_KIND_FIXED_ARRAY &&
            collection_type->fixed_array.element_type)
            element_type = collection_type->fixed_array.element_type;
        else if ((XR_TYPE_IS_ARRAY(collection_type) || XR_TYPE_IS_VIEW(collection_type) ||
                  XR_TYPE_IS_SPAN(collection_type)) &&
                 collection_type->container.element_type)
            element_type = collection_type->container.element_type;
    }
    if (!element_type && collection.as.fixed_array_val.count > 0 &&
        !collection.as.fixed_array_val.is_byte_blob)
        element_type = xa_type_from_ct_value(ctx, &collection.as.fixed_array_val.elements[0]);
    if (!element_type && collection.as.fixed_array_val.is_byte_blob)
        element_type = xr_type_new_int_width(ctx->analyzer->isolate, XR_NATIVE_U8);
    if (!element_type)
        element_type = xr_type_new_unknown(ctx->analyzer->isolate);

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, stmt);
    XaComptimeBlockFlow result = xa_comptime_block_flow_normal();
    XaLoopScope loop_scope;
    xa_loop_scope_push(ctx, &loop_scope, fi->label, stmt);

    XrType *item_type = fi->is_keyvalue ? xr_type_new_int(ctx->analyzer->isolate) : element_type;
    XrType *value_type = element_type;
    XaSymbol *item_sym =
        xa_ensure_comptime_for_in_symbol(ctx, stmt, fi->item_name, &fi->item_symbol_id, item_type);
    XaSymbol *value_sym = fi->is_keyvalue && fi->value_name
                              ? xa_ensure_comptime_for_in_symbol(ctx, stmt, fi->value_name,
                                                                 &fi->value_symbol_id, value_type)
                              : NULL;

    int count = collection.as.fixed_array_val.count;
    for (int i = 0; i < count; i++) {
        XrCtValue index_value = {.kind = XR_CT_INT, .as.int_val = i};
        XrCtValue byte_element = {0};
        XrCtValue *element = NULL;
        if (collection.as.fixed_array_val.is_byte_blob) {
            byte_element.kind = XR_CT_INT;
            byte_element.as.int_val = collection.as.fixed_array_val.byte_blob[i];
            element = &byte_element;
        } else {
            element = &collection.as.fixed_array_val.elements[i];
        }
        if (fi->is_keyvalue) {
            xa_set_comptime_for_in_symbol_value(ctx, item_sym, &index_value, item_type);
            xa_set_comptime_for_in_symbol_value(ctx, value_sym, element, value_type);
        } else {
            xa_set_comptime_for_in_symbol_value(ctx, item_sym, element, item_type);
        }

        int body_diag_count = ctx->analyzer->diagnostic_count;
        XaComptimeBlockFlow flow = xa_comptime_block_flow_normal();
        if (fi->body)
            flow = xa_visit_comptime_block_nested_block(ctx, fi->body, ret);
        if (ctx->analyzer->diagnostic_count > body_diag_count)
            goto done;
        if (flow.kind == XA_COMPTIME_FLOW_BREAK) {
            if (xa_comptime_block_flow_targets_loop(&flow, fi->label))
                goto done;
            result = flow;
            goto done;
        }
        if (flow.kind == XA_COMPTIME_FLOW_CONTINUE) {
            if (xa_comptime_block_flow_targets_loop(&flow, fi->label))
                continue;
            result = flow;
            goto done;
        }
        if (flow.kind == XA_COMPTIME_FLOW_RETURN) {
            result = flow;
            goto done;
        }
    }

done:
    xa_loop_scope_pop(ctx, &loop_scope);
    xa_analyzer_exit_scope(ctx->analyzer);
    return result;
}

static bool xa_ct_value_is_comptime_block_runtime_value(const XrCtValue *value) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_STRING:
        case XR_CT_CHAR:
        case XR_CT_NULL:
        case XR_CT_FIXED_ARRAY:
        case XR_CT_TUPLE:
        case XR_CT_STRUCT_VALUE:
            return true;
        default:
            return false;
    }
}

static XaComptimeBlockFlow xa_visit_comptime_block_return_stmt(XaInferContext *ctx, AstNode *stmt,
                                                               ReturnStmtNode *return_stmt,
                                                               XaComptimeBlockReturn *ret) {
    if (!ctx || !stmt || !return_stmt)
        return xa_comptime_block_flow_normal();
    if (!ret || !ret->allow_return) {
        xa_report_comptime_block_error(ctx, stmt,
                                       "comptime return can only be used in a comptime block "
                                       "expression");
        return xa_comptime_block_flow_normal();
    }
    if (return_stmt->value_count != 1 || !return_stmt->values || !return_stmt->values[0]) {
        xa_report_comptime_block_error(
            ctx, stmt, "comptime block expression return expects exactly one value");
        return xa_comptime_block_flow_normal();
    }

    AstNode *value_expr = return_stmt->values[0];
    xa_clear_ct_value_cache_expr(ctx->analyzer, value_expr);
    XrType *value_type = xa_visit_infer_expr(ctx, value_expr);

    XrCtValue value = {0};
    const char *err = NULL;
    if (!xa_consteval_expr(ctx->analyzer, value_expr, &value, &err)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "comptime block return value must be evaluable at compile time%s%s",
                 err ? ": " : "", err ? err : "");
        xa_report_comptime_block_error(ctx, value_expr, msg);
        return xa_comptime_block_flow_normal();
    }
    if (!xa_ct_value_is_comptime_block_runtime_value(&value)) {
        xa_report_comptime_block_error(
            ctx, value_expr,
            "comptime block expression return value must be a scalar, fixed-array, tuple, or "
            "struct compile-time value in this phase");
        return xa_comptime_block_flow_normal();
    }

    ret->has_value = true;
    ret->value = value;
    ret->type = value_type ? value_type : xr_type_new_unknown(NULL);
    return xa_comptime_block_flow(XA_COMPTIME_FLOW_RETURN, NULL, stmt);
}

static XaComptimeBlockFlow xa_visit_comptime_block_statement(XaInferContext *ctx, AstNode *stmt,
                                                             XaComptimeBlockReturn *ret) {
    if (!ctx || !stmt)
        return xa_comptime_block_flow_normal();

    if (stmt->type == AST_CONST_DECL || stmt->type == AST_VAR_DECL) {
        VarDeclNode *var = &stmt->as.var_decl;
        if (ctx->analyzer && ctx->analyzer->current_scope && var->name &&
            !xa_scope_lookup_local(ctx->analyzer->current_scope, var->name)) {
            var->symbol_id = 0;
            xa_visit_collect_var_decl(ctx, stmt);
        }
        xa_visit_infer_stmt(ctx, stmt);
        return xa_comptime_block_flow_normal();
    }
    if (stmt->type == AST_BLOCK) {
        return xa_visit_comptime_block_nested_block(ctx, stmt, ret);
    }
    if (stmt->type == AST_IF_STMT) {
        return xa_visit_comptime_block_if_stmt(ctx, stmt, &stmt->as.if_stmt, ret);
    }
    if (stmt->type == AST_WHILE_STMT) {
        return xa_visit_comptime_block_while_stmt(ctx, stmt, &stmt->as.while_stmt, ret);
    }
    if (stmt->type == AST_FOR_STMT) {
        return xa_visit_comptime_block_for_stmt(ctx, stmt, &stmt->as.for_stmt, ret);
    }
    if (stmt->type == AST_FOR_IN_STMT) {
        return xa_visit_comptime_block_for_in_stmt(ctx, stmt, &stmt->as.for_in_stmt, ret);
    }
    if (stmt->type == AST_RETURN_STMT) {
        return xa_visit_comptime_block_return_stmt(ctx, stmt, &stmt->as.return_stmt, ret);
    }
    if (stmt->type == AST_BREAK_STMT) {
        return xa_comptime_block_flow(XA_COMPTIME_FLOW_BREAK, stmt->as.break_stmt.label, stmt);
    }
    if (stmt->type == AST_CONTINUE_STMT) {
        return xa_comptime_block_flow(XA_COMPTIME_FLOW_CONTINUE, stmt->as.continue_stmt.label,
                                      stmt);
    }

    AstNode *expr = stmt->type == AST_EXPR_STMT ? stmt->as.expr_stmt : stmt;
    if (expr && expr->type == AST_ASSIGNMENT) {
        xa_visit_comptime_block_assignment(ctx, expr, &expr->as.assignment);
        return xa_comptime_block_flow_normal();
    }
    if (expr && expr->type == AST_COMPOUND_ASSIGNMENT) {
        xa_visit_comptime_block_compound_assignment(ctx, expr, &expr->as.compound_assignment);
        return xa_comptime_block_flow_normal();
    }

    if (!expr || expr->type != AST_CALL_EXPR || !expr->as.call_expr.callee ||
        expr->as.call_expr.callee->type != AST_VARIABLE) {
        xa_report_comptime_block_error(ctx, stmt, xa_comptime_block_supported_message());
        return xa_comptime_block_flow_normal();
    }

    CallExprNode *call = &expr->as.call_expr;
    const char *name = call->callee->as.variable.name;
    if (name && strcmp(name, "compile_assert") == 0) {
        xa_visit_comptime_compile_assert(ctx, expr, call);
    } else if (name && strcmp(name, "compile_error") == 0) {
        xa_visit_comptime_compile_error(ctx, expr, call);
    } else {
        xa_report_comptime_block_error(ctx, expr, xa_comptime_block_supported_message());
    }
    return xa_comptime_block_flow_normal();
}

XrType *xa_visit_comptime_block_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || !xa_is_comptime_block_expr(node))
        return xr_type_new_unknown(NULL);

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
    int saved_comptime_depth = ctx->comptime_block_depth;
    XaLoopScope *saved_loop_scope = ctx->loop_scope;
    int saved_loop_depth = ctx->loop_depth;
    ctx->comptime_block_depth++;
    ctx->loop_scope = NULL;
    ctx->loop_depth = 0;
    int start_diag_count = ctx->analyzer->diagnostic_count;

    XaComptimeBlockReturn ret = {.allow_return = true};
    BlockNode *block = &node->as.comptime_expr.expr->as.block;
    for (int i = 0; i < block->count; i++) {
        AstNode *stmt = block->statements[i];
        XaComptimeBlockFlow flow = xa_visit_comptime_block_statement(ctx, stmt ? stmt : node, &ret);
        if (flow.kind == XA_COMPTIME_FLOW_BREAK || flow.kind == XA_COMPTIME_FLOW_CONTINUE) {
            xa_report_unhandled_comptime_loop_control(ctx, &flow, stmt ? stmt : node);
            break;
        }
        if (flow.kind == XA_COMPTIME_FLOW_RETURN)
            break;
    }

    ctx->comptime_block_depth = saved_comptime_depth;
    ctx->loop_scope = saved_loop_scope;
    ctx->loop_depth = saved_loop_depth;
    xa_analyzer_exit_scope(ctx->analyzer);

    if (ret.has_value) {
        xa_analyzer_set_node_ct_value(ctx->analyzer, node, &ret.value);
        return ret.type ? ret.type : xr_type_new_unknown(NULL);
    }

    if (ctx->analyzer->diagnostic_count == start_diag_count) {
        xa_report_comptime_block_error(ctx, node,
                                       "comptime block expression must return exactly one value");
    }
    return xr_type_new_unknown(NULL);
}

static void xa_visit_comptime_block_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || !xa_is_comptime_block_expr(node))
        return;

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
    int saved_comptime_depth = ctx->comptime_block_depth;
    XaLoopScope *saved_loop_scope = ctx->loop_scope;
    int saved_loop_depth = ctx->loop_depth;
    ctx->comptime_block_depth++;
    ctx->loop_scope = NULL;
    ctx->loop_depth = 0;

    BlockNode *block = &node->as.comptime_expr.expr->as.block;
    for (int i = 0; i < block->count; i++) {
        AstNode *stmt = block->statements[i];
        XaComptimeBlockFlow flow = xa_visit_comptime_block_statement(ctx, stmt ? stmt : node, NULL);
        if (flow.kind == XA_COMPTIME_FLOW_BREAK || flow.kind == XA_COMPTIME_FLOW_CONTINUE) {
            xa_report_unhandled_comptime_loop_control(ctx, &flow, stmt ? stmt : node);
            break;
        }
        if (flow.kind == XA_COMPTIME_FLOW_RETURN) {
            xa_report_comptime_block_error(ctx, flow.node ? flow.node : (stmt ? stmt : node),
                                           "comptime return can only be used in a comptime block "
                                           "expression");
            break;
        }
    }

    ctx->comptime_block_depth = saved_comptime_depth;
    ctx->loop_scope = saved_loop_scope;
    ctx->loop_depth = saved_loop_depth;
    xa_analyzer_exit_scope(ctx->analyzer);
}

XrType *xa_visit_infer_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *result;

    switch (node->type) {
        case AST_LITERAL_INT:
            result = xr_type_new_int(NULL);
            break;
        case AST_LITERAL_FLOAT:
            result = xr_type_new_float(NULL);
            break;
        case AST_LITERAL_RUNE:
            result = xr_type_new_rune(NULL);
            break;
        case AST_LITERAL_STRING:
            result = xr_type_new_string(NULL);
            break;
        case AST_FIXED_BYTES_LITERAL: {
            XrType *byte_type = xr_type_new_int_width(ctx->analyzer->isolate, XR_NATIVE_U8);
            size_t length = node->as.fixed_bytes_literal.payload_length +
                            (node->as.fixed_bytes_literal.append_nul ? 1u : 0u);
            result = xr_type_new_fixed_array(ctx->analyzer->isolate, byte_type, (int) length);
            break;
        }
        case AST_TEMPLATE_STRING:
            xa_freestanding_report_unavailable(ctx, node, "template string",
                                               "string interpolation allocates formatted text");
            /* Visit all interpolation parts to resolve variable symbol_ids. */
            for (int ti = 0; ti < node->as.template_str.part_count; ti++) {
                if (node->as.template_str.parts[ti])
                    xa_visit_infer_expr(ctx, node->as.template_str.parts[ti]);
            }
            result = xr_type_new_string(NULL);
            break;
        case AST_LITERAL_BIGINT:
            xa_freestanding_report_unavailable(ctx, node, "BigInt literal",
                                               "BigInt values require hosted allocation");
            result = xr_type_new_bigint(ctx->analyzer->isolate);
            break;
        case AST_LITERAL_REGEX:
            xa_freestanding_report_unavailable(ctx, node, "regex literal",
                                               "regex compilation is hosted-only");
            result = xr_type_new_regex(ctx->analyzer->isolate);
            break;
        case AST_LITERAL_NULL:
            result = xr_type_new_null(NULL);
            break;
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            result = xr_type_new_bool(NULL);
            break;
        case AST_VARIABLE:
            result = xa_visit_variable(ctx, node);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
            result = xa_visit_binary(ctx, node);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            result = xa_visit_unary(ctx, node);
            break;
        case AST_CALL_EXPR:
            result = xa_visit_call(ctx, node);
            break;
        case AST_MEMBER_ACCESS:
            result = xa_visit_member_access(ctx, node);
            break;
        case AST_INDEX_GET:
            result = xa_visit_index_get(ctx, node);
            break;
        case AST_ARRAY_LITERAL:
            result = xa_visit_array_literal(ctx, node);
            break;
        case AST_TUPLE_LITERAL:
            result = xa_visit_tuple_literal(ctx, node);
            break;
        case AST_MAP_LITERAL:
            result = xa_visit_map_literal(ctx, node);
            break;
        case AST_OBJECT_LITERAL:
            result = xa_visit_object_literal(ctx, node);
            break;
        case AST_NEW_EXPR:
            result = xa_visit_new_expr(ctx, node);
            break;
        case AST_STRUCT_LITERAL:
            result = xa_visit_struct_literal(ctx, node);
            break;
        case AST_TERNARY:
            result = xa_visit_ternary(ctx, node);
            break;
        case AST_FUNCTION_EXPR:
            result = xa_visit_function_expr(ctx, node);
            break;
        case AST_GROUPING:
            result = xa_visit_infer_expr(ctx, node->as.grouping);
            break;
        case AST_COMPTIME_EXPR:
            result = xa_visit_comptime_expr(ctx, node);
            break;
        case AST_GO_EXPR:
            result = xa_visit_go_expr(ctx, node);
            break;
        case AST_AWAIT_EXPR:
            result = xa_visit_await_expr(ctx, node);
            break;
        case AST_UNSAFE_EXPR:
            result = xa_visit_unsafe_expr(ctx, node);
            break;
        case AST_MATCH_EXPR:
            result = xa_visit_match_expr(ctx, node);
            break;
        case AST_NULLISH_COALESCE:
            result = xa_visit_nullish_coalesce(ctx, node);
            break;
        case AST_OPTIONAL_CHAIN:
            result = xa_visit_optional_chain(ctx, node);
            break;
        case AST_FORCE_UNWRAP:
            result = xa_visit_force_unwrap(ctx, node);
            break;
        case AST_AS_EXPR:
            result = xa_visit_as_expr(ctx, node);
            break;
        case AST_IS_EXPR:
            if (node->as.is_expr.expr)
                xa_visit_infer_expr(ctx, node->as.is_expr.expr);
            result = xr_type_new_bool(NULL);
            break;
        case AST_RANGE:
            if (node->as.range.start)
                xa_visit_infer_expr(ctx, node->as.range.start);
            if (node->as.range.end)
                xa_visit_infer_expr(ctx, node->as.range.end);
            result = xr_type_new_named_instance(ctx->analyzer->isolate, "Range");
            break;
        case AST_SET_LITERAL: {
            xa_freestanding_report_unavailable(ctx, node, "Set literal",
                                               "dynamic containers require hosted allocation");
            XrType *elem = NULL;
            if (ctx->expected_type && ctx->expected_type->kind == XR_KIND_SET &&
                ctx->expected_type->container.element_type) {
                elem = ctx->expected_type->container.element_type;
            }
            if (node->as.set_literal.count == 0 && !elem) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                XaInferVar *var = xa_infer_var_new(ctx, "empty set element", &loc);
                result = xa_infer_var_report_unsolved(
                    ctx, var,
                    "cannot infer element type for empty set literal; add an explicit Set<T> "
                    "annotation or contextual type");
                break;
            }
            /* Visit ALL elements to resolve symbol_ids; infer elem type from first. */
            XrType *saved_expected = ctx->expected_type;
            for (int si = 0; si < node->as.set_literal.count; si++) {
                if (node->as.set_literal.elements[si]) {
                    ctx->expected_type = elem;
                    XrType *et = xa_visit_infer_expr(ctx, node->as.set_literal.elements[si]);
                    xa_check_span_value_escape(ctx, node->as.set_literal.elements[si], et,
                                               "store Slice view in set literal");
                    xa_check_pointer_borrow_escape(ctx, node->as.set_literal.elements[si],
                                                   node->as.set_literal.elements[si], et,
                                                   "store raw pointer borrow in set literal");
                    if (!elem && si == 0)
                        elem = et;
                }
            }
            ctx->expected_type = saved_expected;
            if (!elem)
                elem = xr_type_new_unknown(NULL);
            result = xr_type_new_set(ctx->analyzer->isolate, elem);
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_validate_hashable_key_type(ctx, result, NULL, "set literal", &loc);
            break;
        }
        case AST_CHANNEL_NEW:
            // Visit buffer size expression to resolve variable symbol_ids
            if (node->as.channel_new.buffer_size)
                xa_visit_infer_expr(ctx, node->as.channel_new.buffer_size);
            // Use expected_type if available (from type annotation: Channel<T>)
            if (ctx->expected_type && (ctx->expected_type->kind == XR_KIND_CHANNEL)) {
                result = ctx->expected_type;
            } else {
                result = xr_type_new_channel(ctx->analyzer->isolate, xr_type_new_unknown(NULL));
            }
            break;
        case AST_MOVE_EXPR:
            result = xa_visit_move_expr(ctx, node);
            break;
        case AST_SUPER_CALL: {
            /* Resolve super.method() return type by looking up the method
             * in the base class chain via class_info. */
            bool visited_args = false;
            XaScope *s = ctx->analyzer->current_scope;
            while (s && s->kind != XA_SCOPE_CLASS)
                s = s->parent;
            if (s && s->class_symbol) {
                XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, s->class_symbol);
                if (cl && cl->class_info && cl->class_info->base) {
                    const char *mname = node->as.super_call.method_name;
                    if (mname) {
                        XaSymbol *member = xa_class_info_lookup_member(cl->class_info->base, mname);
                        if (member) {
                            XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                            if (ml && ml->type) {
                                if (XR_TYPE_IS_FUNCTION(ml->type)) {
                                    xa_visit_super_args_for_function(ctx, node, ml->type);
                                    visited_args = true;
                                } else {
                                    xa_visit_super_args_without_contract(ctx, &node->as.super_call);
                                    visited_args = true;
                                }
                                /* Method type is a function type; extract
                                 * return type for the call result. */
                                if (XR_TYPE_IS_FUNCTION(ml->type) &&
                                    ml->type->function.return_type) {
                                    result = ml->type->function.return_type;
                                } else {
                                    result = ml->type;
                                }
                                break;
                            }
                        }
                    } else {
                        xa_check_constructor_visibility(ctx, node, cl->class_info->base);
                        XaSymbol *ctor = xa_class_info_lookup_member(cl->class_info->base,
                                                                     XR_KEYWORD_CONSTRUCTOR);
                        XaSymbolLinks *ctor_links =
                            ctor ? xa_analyzer_get_links(ctx->analyzer, ctor) : NULL;
                        XrType *ctor_type = ctor_links ? ctor_links->type : NULL;
                        if (ctor_type && XR_TYPE_IS_FUNCTION(ctor_type)) {
                            xa_visit_super_args_for_function(ctx, node, ctor_type);
                            visited_args = true;
                        }
                    }
                }
            }
            if (!visited_args)
                xa_visit_super_args_without_contract(ctx, &node->as.super_call);
            result = xr_type_new_unknown(NULL);
            break;
        }
        case AST_THIS_EXPR: {
            /* Walk up scopes to find the enclosing class scope and
             * return the class instance type so this.field resolves. */
            XaScope *s = ctx->analyzer->current_scope;
            while (s && s->kind != XA_SCOPE_CLASS)
                s = s->parent;
            if (s && s->class_symbol) {
                const char *cname = s->class_symbol->name;
                if (cname) {
                    XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, s->class_symbol);
                    if (s->class_symbol->kind == XA_SYM_ENUM) {
                        result = xr_type_new_enum(ctx->analyzer->isolate, cname);
                    } else {
                        bool is_struct = cl && cl->type && cl->type->is_value_type;
                        result = xr_type_new_named_instance(ctx->analyzer->isolate, cname);
                        if (result) {
                            result->instance.class_ref = cl ? cl->class_info : NULL;
                            result->is_value_type = is_struct;
                        }
                    }
                } else {
                    result = xr_type_new_unknown(NULL);
                }
            } else {
                result = xr_type_new_unknown(NULL);
            }
            break;
        }
        case AST_SLICE_EXPR: {
            SliceExprNode *sl = &node->as.slice_expr;
            XrType *src = sl->source ? xa_visit_infer_expr(ctx, sl->source) : NULL;
            if (sl->start)
                xa_visit_infer_expr(ctx, sl->start);
            if (sl->end)
                xa_visit_infer_expr(ctx, sl->end);
            /* Array/Span slices are target-typed view-producing expressions.
             * They no longer choose Span/View/owned semantics by themselves. */
            if (src && (XR_TYPE_IS_ARRAY(src) || XR_TYPE_IS_SPAN(src) || XR_TYPE_IS_VIEW(src) ||
                        src->kind == XR_KIND_FIXED_ARRAY)) {
                XrType *target = ctx->expected_type;
                if (target && XR_TYPE_IS_SPAN(target)) {
                    xa_check_span_borrow_source_stable(ctx, node, sl->source, "slice expression");
                    result = XR_TYPE_IS_UNKNOWN(target->container.element_type)
                                 ? xa_span_type_from_view_source(ctx, src)
                                 : target;
                    if (!xa_span_target_matches_source(result, src)) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[256];
                        snprintf(
                            msg, sizeof(msg),
                            "slice source type '%s' is not compatible with target view type '%s'",
                            xr_type_to_string(src), xr_type_to_string(result));
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    }
                } else if (target && XR_TYPE_IS_VIEW(target) && src->kind != XR_KIND_FIXED_ARRAY) {
                    result = XR_TYPE_IS_UNKNOWN(target->container.element_type)
                                 ? xa_storage_view_type_from_source(ctx, src)
                                 : target;
                    if (!xa_view_target_matches_source(result, src)) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[256];
                        snprintf(
                            msg, sizeof(msg),
                            "slice source type '%s' is not compatible with target view type '%s'",
                            xr_type_to_string(src), xr_type_to_string(result));
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    }
                } else if (target && XR_TYPE_IS_VIEW(target)) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    xa_analyzer_add_diagnostic(
                        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                        "fixed array slices can only produce scoped Span<T>, not View<T>", &loc);
                    result = xr_type_new_unknown(NULL);
                } else if (ctx->allow_view_expr_for_copy) {
                    xa_check_span_borrow_source_stable(ctx, node, sl->source, "slice expression");
                    result = xa_span_type_from_view_source(ctx, src);
                } else {
                    xa_report_view_expr_requires_target(ctx, node, "slice");
                    result = xr_type_new_unknown(NULL);
                }
            } else if (src && XR_TYPE_IS_STRING(src)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                    "string does not support integer indexing or slice syntax; use runes(), "
                    "bytes(), or slice(start, end)",
                    &loc);
                result = xr_type_new_error(ctx->analyzer->isolate);
            } else {
                result = xa_report_invalid_slice_source(ctx, node, src);
            }
            break;
        }
        case AST_ENUM_ACCESS:
            result = node->as.enum_access.enum_name
                         ? xr_type_new_enum(ctx->analyzer->isolate, node->as.enum_access.enum_name)
                         : xr_type_new_unknown(NULL);
            break;
        case AST_SCOPE_BLOCK:
            if (node->as.scope_block.body)
                xa_visit_infer_stmt(ctx, node->as.scope_block.body);
            result = xr_type_new_null(NULL);
            break;
        case AST_CANCELLED_EXPR:
            result = xr_type_new_bool(NULL);
            break;
        default:
            result = xr_type_new_unknown(NULL);
            break;
    }

    // Propagate is_value_type for class/instance types created from type annotations
    // (parser doesn't know if a type name is struct vs class, so is_value_type defaults to false)
    if (result && !result->is_value_type &&
        (result->kind == XR_KIND_CLASS || result->kind == XR_KIND_INSTANCE) &&
        result->instance.class_name) {
        XaSymbol *_vs = xa_scope_lookup(ctx->analyzer->global_scope, result->instance.class_name);
        if (_vs && _vs->kind == XA_SYM_CLASS) {
            XaSymbolLinks *_vl = xa_analyzer_get_links(ctx->analyzer, _vs);
            if (_vl && _vl->type && _vl->type->is_value_type) {
                result->is_value_type = true;
            }
        }
    }

    if (result && XR_TYPE_IS_ERROR(result)) {
        ctx->analyzer->recovery_poison_type_count++;
    } else if (result && XR_TYPE_IS_UNKNOWN(result)) {
        ctx->analyzer->unresolved_inference_count++;
    }

    check_contextual_int_literal_range(ctx, node, ctx->expected_type);

    // Cache inferred type in the analyzer side table for codegen.
    xa_analyzer_set_node_type(ctx->analyzer, node, result);
    return result;
}

/*
 * Unified function body visitor.
 * Handles both named functions (Pass 1 already collected) and
 * function expressions (Pass 1 never reached).
 *
 * Always call xa_visit_collect() on body — it is idempotent.
 * For Pass 1 scopes: symbols already exist, hashmap_set overwrites harmlessly.
 * For new scopes: symbols are registered fresh.
 * Body is always visited directly (no xa_visit_block_stmt) to ensure
 * consistent FUNCTION_SCOPE structure.
 */
void xa_visit_function_body_unified(XaInferContext *ctx, AstNode *body) {
    if (!body)
        return;

    // Collect body declarations (idempotent: safe on both new and reused scopes)
    xa_visit_collect(ctx, body);

    // Visit body statements directly (skip xa_visit_block_stmt) while still
    // exposing a statement cursor for Span last-use analysis.
    xa_visit_inline_statement_sequence_with_cursor(ctx, body);
}

/* Generator return-type recognition. A generator function declares
 * `-> Iterator<T>`, which resolves to the built-in Iterator interface
 * (XR_KIND_INTERFACE, name "Iterator", one type arg). Accept that form and
 * extract T; also accept a structural iterator (class/instance exposing
 * hasNext()/next()) so a generator may be annotated with a concrete iterator
 * type. The global xr_type_is_iterator() is a stub that always returns false
 * (custom-iterator recognition needs analyzer context), so generator typing
 * must use this analyzer-aware helper. */
static bool xa_generator_return_element(XaAnalyzer *analyzer, XrType *rt, XrType **out_elem) {
    if (!rt)
        return false;
    if ((rt->kind == XR_KIND_INTERFACE || rt->kind == XR_KIND_INSTANCE) &&
        rt->instance.class_name && strcmp(rt->instance.class_name, "Iterator") == 0) {
        if (out_elem) {
            *out_elem = (rt->instance.type_arg_count >= 1 && rt->instance.type_args &&
                         rt->instance.type_args[0])
                            ? rt->instance.type_args[0]
                            : xr_type_new_unknown(NULL);
        }
        return true;
    }
    return xa_analyzer_is_iterator(analyzer, rt, out_elem);
}

static void xa_check_borrowed_yield_escape(XaInferContext *ctx, AstNode *yield_node, AstNode *value,
                                           XrType *value_type) {
    if (!ctx || !yield_node || !value || !xa_type_needs_borrow_escape_guard(value_type) ||
        xa_type_contains_span_view(value_type))
        return;

    XaSymbol *root = xa_borrowed_param_root_symbol(ctx, value);
    if (!root)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = value->line ? value->line : yield_node->line,
                      .column = value->column ? value->column : yield_node->column};
    const char *mode = root->passing_mode == XR_PARAM_REF ? "ref" : "in";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot yield borrowed '%s' parameter '%s'; yield an owned value or copy(%s)", mode,
             root->name ? root->name : "?", root->name ? root->name : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

void xa_visit_infer_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    switch (node->type) {
        case AST_PROGRAM:
            xa_visit_inline_statement_sequence_with_cursor(ctx, node);
            break;
        case AST_BLOCK:
            xa_visit_block_stmt(ctx, node);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            xa_visit_var_decl_stmt(ctx, node);
            break;
        case AST_ASSIGNMENT:
            xa_visit_assignment_stmt(ctx, node);
            break;
        case AST_INC:
        case AST_DEC: {
            IncDecNode *id = &node->as.inc;
            XaSymbol *id_sym = xa_scope_lookup(ctx->analyzer->current_scope, id->name);
            if (id_sym)
                id->symbol_id = id_sym->id;
            xa_parallel_capture_check(ctx, node, id_sym, true);
            if (id_sym && (id_sym->is_const || id_sym->is_shared || id_sym->is_owned ||
                           !id_sym->is_rebindable)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[128];
                const char *fmt = id_sym->is_shared  ? "Cannot modify shared binding '%s'"
                                  : id_sym->is_owned ? "Cannot modify owned binding '%s'"
                                                     : "Cannot modify const '%s'";
                snprintf(msg, sizeof(msg), fmt, id->name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
            break;
        }
        case AST_COMPOUND_ASSIGNMENT: {
            // Infer value expression so its type is recorded in the side table
            CompoundAssignmentNode *ca = &node->as.compound_assignment;
            XrType *ca_value_type = ca->value ? xa_visit_infer_expr(ctx, ca->value) : NULL;
            // Visit object expression for member compound assign (obj.field += ...)
            XrType *ca_obj_type = NULL;
            if (ca->object)
                ca_obj_type = xa_visit_infer_expr(ctx, ca->object);
            if (ca_obj_type && XR_TYPE_IS_POINTER(ca_obj_type) && ca_obj_type->ptr_is_c_view &&
                !ca_obj_type->ptr_is_mut) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                    "cannot assign through a const mem.view created from Ptr<T>; use MutPtr<T>",
                    &loc);
            }
            // Tuples are immutable: reject compound assignment on tuple fields
            if (ca_obj_type && XR_TYPE_IS_TUPLE(ca_obj_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Cannot assign to tuple field '.%s': tuples are immutable",
                         ca->name ? ca->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TUPLE_IMMUTABLE, msg, &loc);
                break;
            }
            // Check const/in-param immutability
            XaSymbol *ca_sym =
                ca->object ? NULL : xa_scope_lookup(ctx->analyzer->current_scope, ca->name);
            if (ca_sym)
                ca->symbol_id = ca_sym->id;
            xa_parallel_capture_check(ctx, node, ca_sym, true);
            XrType *ca_target_type = NULL;
            if (!ca->object && ca_sym)
                ca_target_type = xa_analyzer_get_type(ctx->analyzer, ca_sym);
            if (ca->object && ca_obj_type && ca->name) {
                XaSymbolLinks *class_links = NULL;
                XrClassInfo *class_info = member_set_class_info(ctx, ca_obj_type, &class_links);
                if (class_info) {
                    XaSymbol *field = xa_class_info_lookup_member(class_info, ca->name);
                    if (field) {
                        XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, field);
                        if (fl && fl->type && !XR_TYPE_IS_UNKNOWN(fl->type))
                            ca_target_type = member_set_substitute_field_type(
                                ctx, ca_obj_type, class_links, fl->type);
                    }
                }
            }
            if (ca_target_type)
                xa_analyzer_set_node_type(ctx->analyzer, node, ca_target_type);
            if (ca->op == TK_MOD_ASSIGN && ca_target_type && ca_value_type &&
                !XR_TYPE_IS_UNKNOWN(ca_target_type) && !XR_TYPE_IS_UNKNOWN(ca_value_type) &&
                (xa_type_contains_float(ca_target_type) || xa_type_contains_float(ca_value_type))) {
                xa_report_float_modulo_error(ctx, node, ca_target_type, ca_value_type);
            }
            if (ca_sym && (ca_sym->is_const || ca_sym->is_shared || ca_sym->is_owned ||
                           !ca_sym->is_rebindable)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[128];
                const char *fmt = ca_sym->is_shared  ? "Cannot assign to shared binding '%s'"
                                  : ca_sym->is_owned ? "Cannot assign to owned binding '%s'"
                                                     : "Cannot assign to const '%s'";
                snprintf(msg, sizeof(msg), fmt, ca->name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
            if (ca->object) {
                XaSymbol *root = xa_root_variable_symbol_for_expr(ctx, ca->object);
                bool readonly_object = xr_type_is_const(ca_obj_type);
                if ((root &&
                     (root->is_readonly_binding || xa_symbol_has_shared_provenance(root))) ||
                    readonly_object) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[192];
                    const char *label = root && xa_symbol_has_shared_provenance(root)
                                            ? "shared binding"
                                        : root && root->is_readonly_binding ? "const binding"
                                                                            : "readonly value";
                    snprintf(msg, sizeof(msg), "Cannot modify field '%s' of %s '%s'",
                             ca->name ? ca->name : "?", label,
                             root && root->name ? root->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                }
            }
            break;
        }
        case AST_MEMBER_SET: {
            // Infer types for member set expression
            MemberSetNode *ms = &node->as.member_set;
            XrType *obj_type = NULL;
            if (ms->object && ms->member) {
                char target_path[256];
                XaSymbol *root = member_set_out_field_path_symbol(ctx, ms->object, target_path,
                                                                  sizeof(target_path));
                if (root && target_path[0] != '\0' &&
                    member_set_out_field_path_append(target_path, sizeof(target_path), ".") &&
                    member_set_out_field_path_append(target_path, sizeof(target_path),
                                                     ms->member)) {
                    XaSymbolLinks *root_links = xa_analyzer_get_links(ctx->analyzer, root);
                    if (root && root->kind == XA_SYM_PARAMETER &&
                        root->passing_mode == XR_PARAM_OUT && root_links &&
                        !root_links->is_definitely_assigned) {
                        obj_type = member_set_out_field_object_type_without_receiver_read(
                            ctx, ms->object, target_path);
                    }
                }
            }
            if (!obj_type)
                obj_type = xa_visit_infer_expr(ctx, ms->object);
            bool is_c_view = obj_type && XR_TYPE_IS_POINTER(obj_type) && obj_type->ptr_is_c_view;
            if (is_c_view && !obj_type->ptr_is_mut) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                    "cannot assign through a const mem.view created from Ptr<T>; use MutPtr<T>",
                    &loc);
            }
            XaSymbol *readonly_root = xa_root_variable_symbol_for_expr(ctx, ms->object);
            bool readonly_object = xr_type_is_const(obj_type);
            if ((readonly_root && (readonly_root->is_readonly_binding ||
                                   xa_symbol_has_shared_provenance(readonly_root))) ||
                readonly_object) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[192];
                const char *label = readonly_root && xa_symbol_has_shared_provenance(readonly_root)
                                        ? "shared binding"
                                    : readonly_root && readonly_root->is_readonly_binding
                                        ? "const binding"
                                        : "readonly value";
                snprintf(msg, sizeof(msg), "Cannot modify field '%s' of %s '%s'",
                         ms->member ? ms->member : "?", label,
                         readonly_root && readonly_root->name ? readonly_root->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }

            // Tuples are immutable: reject any field assignment
            if (obj_type && XR_TYPE_IS_TUPLE(obj_type)) {
                xa_visit_infer_expr(ctx, ms->value);
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Cannot assign to tuple field '.%s': tuples are immutable",
                         ms->member ? ms->member : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TUPLE_IMMUTABLE, msg, &loc);
                break;
            }

            // Bidirectional inference: propagate field declared type to value
            XrType *saved_expected = ctx->expected_type;
            XrType *member_type = NULL;
            if (ms->member) {
                XaSymbolLinks *class_links = NULL;
                XrClassInfo *class_info = member_set_class_info(ctx, obj_type, &class_links);
                if (class_info) {
                    XrClassInfo *field_owner = NULL;
                    XaSymbol *field =
                        xa_class_info_lookup_member_owner(class_info, ms->member, &field_owner);
                    if (field) {
                        // Enforce private/protected visibility on the write target.
                        xa_check_member_visibility(ctx, node, field, field_owner);

                        // const fields are immutable: only the declaring class's
                        // constructor may assign them, and only through `this`.
                        if (field->is_const) {
                            bool in_owner_ctor = ctx->current_method_is_constructor &&
                                                 ctx->current_class_info != NULL &&
                                                 ctx->current_class_info == field_owner;
                            bool through_this = ms->object && ms->object->type == AST_THIS_EXPR;
                            if (!in_owner_ctor || !through_this) {
                                XrLocation loc = {.file = ctx->file_path,
                                                  .line = node->line,
                                                  .column = node->column};
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                         "cannot assign to const field '%s'; const fields are set "
                                         "only in the constructor",
                                         ms->member);
                                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                           XR_ERR_ANALYZE_CONST_FIELD, msg, &loc);
                            }
                        }
                        XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, field);
                        if (fl && fl->type && !XR_TYPE_IS_UNKNOWN(fl->type)) {
                            member_type = member_set_substitute_field_type(ctx, obj_type,
                                                                           class_links, fl->type);
                            if (member_type && !XR_TYPE_IS_UNKNOWN(member_type))
                                ctx->expected_type = member_type;
                        }
                    }
                    if (is_c_view && class_info->struct_layout) {
                        int field_index =
                            member_set_layout_field_index(class_info->struct_layout, ms->member);
                        if (field_index >= 0) {
                            XrAggregateFieldLayout *layout_field =
                                &class_info->struct_layout->fields[field_index];
                            if (layout_field->is_flexible ||
                                layout_field->native_type == XR_NATIVE_ARRAY ||
                                layout_field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
                                XrLocation loc = {.file = ctx->file_path,
                                                  .line = node->line,
                                                  .column = node->column};
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                         "C view field '%s.%s' cannot be assigned as a whole; "
                                         "write through a validated element or nested projection",
                                         class_info->name ? class_info->name : "?",
                                         ms->member ? ms->member : "?");
                                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                            }
                        }
                    }
                }
            }
            char owner_path[512] = {0};
            XaSymbol *owner = xa_span_borrow_owner_path_for_member_write(
                ctx, ms->object, ms->member, member_type, owner_path, sizeof(owner_path));
            if (owner) {
                xa_check_active_span_borrow_owner_path_mutation(
                    ctx, node, owner, owner_path[0] ? owner_path : NULL, "reassigning owner field");
            }
            XrType *value_type = xa_visit_infer_expr(ctx, ms->value);
            ctx->expected_type = saved_expected;
            xa_assign_check_type(ctx, node, member_type, value_type, ms->member, "member");
            xa_check_span_value_escape(ctx, node, value_type, "store Slice view into a member");
            xa_check_pointer_borrow_escape(ctx, node, ms->value, value_type,
                                           "store raw pointer borrow into a member");
            if (xa_type_needs_borrow_escape_guard(value_type)) {
                XaSymbol *borrowed_root = xa_borrowed_param_root_symbol(ctx, ms->value);
                if (borrowed_root) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    const char *mode = borrowed_root->passing_mode == XR_PARAM_REF ? "ref" : "in";
                    const char *name = borrowed_root->name ? borrowed_root->name : "?";
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot store borrowed '%s' parameter '%s' into member '%s'; "
                             "store an owned value or copy(%s)",
                             mode, name, ms->member ? ms->member : "?", name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
            if (XR_TYPE_HAS_OBJECT_SHAPE(obj_type) && obj_type->object.field_count > 0 &&
                ms->member) {
                int field_idx = object_shape_field_index_local(obj_type, ms->member);
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                if (field_idx >= 0) {
                    bool readonly = obj_type->object.field_readonly
                                        ? obj_type->object.field_readonly[field_idx]
                                        : false;
                    if (readonly) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "无法修改只读字段 '%s.%s'（const 修饰）",
                                 object_shape_type_label_local(obj_type), ms->member);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                    }
                } else if (obj_type->object.is_sealed) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "类型 '%s' 不允许添加字段 '%s'",
                             object_shape_type_label_local(obj_type), ms->member);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
            // Check in-parameter immutability: v.x = ... where v is 'in' param
            if (ms->object && ms->object->type == AST_VARIABLE) {
                const char *obj_name = ms->object->as.variable.name;
                XaSymbol *obj_sym = xa_scope_lookup(ctx->analyzer->current_scope, obj_name);
                if (obj_sym && obj_sym->kind == XA_SYM_PARAMETER &&
                    obj_sym->passing_mode == XR_PARAM_IN) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Cannot modify field '%s' of 'in' parameter '%s' (readonly reference)",
                             ms->member, obj_name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                }
            }
            {
                char path[256];
                path[0] = '\0';
                XaSymbol *root = NULL;
                if (ms->object && ms->member) {
                    root = member_set_out_field_path_symbol(ctx, ms->object, path, sizeof(path));
                    if (root && path[0] != '\0' &&
                        (!member_set_out_field_path_append(path, sizeof(path), ".") ||
                         !member_set_out_field_path_append(path, sizeof(path), ms->member))) {
                        root = NULL;
                        path[0] = '\0';
                    }
                }
                if (root && path[0] != '\0') {
                    XaSymbolLinks *root_links = xa_analyzer_get_links(ctx->analyzer, root);
                    if (root->kind == XA_SYM_PARAMETER && root->passing_mode == XR_PARAM_OUT &&
                        root_links && !root_links->is_definitely_assigned) {
                        xa_symbol_links_mark_out_field_assigned(root_links, path);
                        if (ms->object && ms->object->type == AST_MEMBER_ACCESS && obj_type) {
                            char object_path[256];
                            XaSymbol *object_root = member_set_out_field_path_symbol(
                                ctx, ms->object, object_path, sizeof(object_path));
                            if (object_root == root && object_path[0] != '\0') {
                                XaSymbolLinks *class_links = NULL;
                                XrClassInfo *object_info =
                                    member_set_class_info(ctx, obj_type, &class_links);
                                (void) class_links;
                                xa_symbol_links_mark_out_field_assigned_if_all_direct_fields_assigned_for_class(
                                    root_links, object_path, object_info);
                            }
                        }
                        XrType *root_type = xa_analyzer_get_type(ctx->analyzer, root);
                        if (!root_type)
                            root_type =
                                root_links->type ? root_links->type : root_links->declared_type;
                        bool marked_whole =
                            root_type
                                ? xa_symbol_links_mark_out_whole_assigned_if_all_direct_fields_assigned_for_type(
                                      root_links, root->name, root_type)
                                : false;
                        if (!marked_whole && ms->object && ms->object->type == AST_VARIABLE &&
                            ms->object->as.variable.symbol_id == root->id) {
                            XaSymbolLinks *class_links = NULL;
                            XrClassInfo *root_info =
                                member_set_class_info(ctx, obj_type, &class_links);
                            (void) class_links;
                            xa_symbol_links_mark_out_whole_assigned_if_all_direct_fields_assigned_for_class(
                                root_links, root->name, root_info);
                        }
                    }
                }
            }
            break;
        }
        case AST_IF_STMT:
            xa_visit_if_stmt(ctx, node);
            break;
        case AST_WHILE_STMT:
            xa_visit_while_stmt(ctx, node);
            break;
        case AST_FOR_STMT:
            xa_visit_for_stmt(ctx, node);
            break;
        case AST_RETURN_STMT:
            xa_visit_return_stmt(ctx, node);
            break;
        case AST_EXPR_STMT: {
            AstNode *inner = node->as.expr_stmt;
            if (xa_is_comptime_block_expr(inner)) {
                xa_visit_comptime_block_stmt(ctx, inner);
                break;
            }
            // Check in-parameter immutability before normal inference
            if (inner && inner->type == AST_ASSIGNMENT) {
                AssignmentNode *a = &inner->as.assignment;
                XaSymbol *s = xa_scope_lookup(ctx->analyzer->current_scope, a->name);
                if (s && s->kind == XA_SYM_PARAMETER && s->passing_mode == XR_PARAM_IN) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = inner->line, .column = inner->column};
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "Cannot assign to 'in' parameter '%s' (readonly reference)", a->name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                }
            } else if (inner && inner->type == AST_MEMBER_SET) {
                MemberSetNode *ms = &inner->as.member_set;
                if (ms->object && ms->object->type == AST_VARIABLE) {
                    const char *obj_name = ms->object->as.variable.name;
                    XaSymbol *s = xa_scope_lookup(ctx->analyzer->current_scope, obj_name);
                    if (s && s->kind == XA_SYM_PARAMETER && s->passing_mode == XR_PARAM_IN) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = inner->line, .column = inner->column};
                        char msg[256];
                        snprintf(
                            msg, sizeof(msg),
                            "Cannot modify field '%s' of 'in' parameter '%s' (readonly reference)",
                            ms->member, obj_name);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                    }
                }
            }
            if (inner && (inner->type == AST_MEMBER_SET || inner->type == AST_ASSIGNMENT ||
                          inner->type == AST_COMPOUND_ASSIGNMENT || inner->type == AST_INC ||
                          inner->type == AST_DEC || inner->type == AST_INDEX_SET)) {
                xa_visit_infer_stmt(ctx, inner);
            } else {
                xa_visit_infer_expr(ctx, inner);
                xa_warn_discarded_sys_thread_spawn(ctx, inner);
                xa_warn_discarded_sys_os_resource_factory(ctx, inner);
            }
            break;
        }
        case AST_FUNCTION_DECL: {
            // Already collected in pass 1, infer body
            FunctionDeclNode *fn_decl = &node->as.function_decl;

            // Save/restore return type state for nested functions
            XrType **saved_return_types = ctx->return_types;
            int saved_return_count = ctx->return_type_count;
            int saved_return_cap = ctx->return_type_capacity;
            uint8_t saved_return_storage_owner = ctx->return_storage_owner;
            bool saved_return_storage_known = ctx->return_storage_known;
            bool saved_return_storage_mixed = ctx->return_storage_mixed;
            bool saved_return_storage_unknown = ctx->return_storage_unknown;
            ctx->return_types = NULL;
            ctx->return_type_count = 0;
            ctx->return_type_capacity = 0;
            ctx->return_storage_owner = XR_STORAGE_NONE;
            ctx->return_storage_known = false;
            ctx->return_storage_mixed = false;
            ctx->return_storage_unknown = false;

            // Isolate flow graph: each function gets a fresh start node
            // so that flow facts from sibling/parent functions (e.g.
            // `var name = p?.name`) do not leak into this function body.
            XaFlowNode *saved_flow = NULL;
            XrFlowLabel *saved_break = NULL;
            XrFlowLabel *saved_continue = NULL;
            XrFlowLabel *saved_return = NULL;
            XrFlowLabel *saved_exception = NULL;
            if (ctx->flow) {
                saved_flow = ctx->flow->current_flow;
                saved_break = ctx->flow->current_break_target;
                saved_continue = ctx->flow->current_continue_target;
                saved_return = ctx->flow->current_return_target;
                saved_exception = ctx->flow->current_exception_target;
                xa_flow_create_start(ctx->flow);
                ctx->flow->current_break_target = NULL;
                ctx->flow->current_continue_target = NULL;
                ctx->flow->current_return_target = NULL;
                ctx->flow->current_exception_target = NULL;
            }

            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);

            // Call-site inference feedback: update unannotated parameter types
            // with types inferred from call-sites during Pass 2.
            if (fn_decl->name) {
                XaSymbol *fn_sym =
                    xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                XaSymbolLinks *fn_links =
                    fn_sym ? xa_analyzer_get_links(ctx->analyzer, fn_sym) : NULL;
                if (fn_links && fn_links->inferred_param_types) {
                    for (int pi = 0;
                         pi < fn_decl->param_count && pi < fn_links->inferred_param_count; pi++) {
                        XrType *inferred = fn_links->inferred_param_types[pi];
                        if (!inferred || XR_TYPE_IS_UNKNOWN(inferred))
                            continue;
                        XrParamNode *p = fn_decl->params[pi];
                        if (!p || !p->name || p->type)
                            continue;  // skip annotated params
                        XaSymbol *param_sym =
                            xa_scope_lookup_local(ctx->analyzer->current_scope, p->name);
                        if (param_sym) {
                            XaSymbolLinks *pl = xa_analyzer_get_links(ctx->analyzer, param_sym);
                            if (pl && (!pl->type || XR_TYPE_IS_UNKNOWN(pl->type))) {
                                pl->type = inferred;
                            }
                        }
                    }
                }
                xa_apply_param_storage_requirements_to_scope(ctx, fn_links);
            }

            // Set expected_return_type for return type checking.
            // Named functions: explicit annotation → use it; omitted → void.
            // This ensures xa_visit_return_stmt catches type mismatches
            // (e.g. return true in :int fn, or return 42 in :void fn).
            XrType *saved_expected_ret = ctx->expected_return_type;
            if (fn_decl->return_type) {
                ctx->expected_return_type =
                    xr_tref_resolve_in_analyzer(ctx->analyzer, fn_decl->return_type);
            } else {
                ctx->expected_return_type = xr_type_new_unit(NULL);
            }

            // Generator detection: `yield expr` in the body makes this a
            // generator. Scope the flag per function (nested functions reset it).
            bool saved_has_yield = ctx->current_fn_has_yield;
            ctx->current_fn_has_yield = false;

            // Unified body visitor: idempotent collect + direct traversal
            xa_visit_function_body_unified(ctx, fn_decl->body);
            xa_check_out_params_assigned_at_function_exit(ctx, ctx->analyzer->current_scope,
                                                          fn_decl->body);

            if (ctx->current_fn_has_yield) {
                /* `yield expr` in the body makes this a generator. The required
                 * `-> Iterator<T>` return type is enforced at the yield site
                 * (single diagnostic), so here we only mark the function. */
                fn_decl->is_generator = true;
            }
            ctx->current_fn_has_yield = saved_has_yield;

            // Re-check inferred_param_types after body analysis: recursive calls
            // inside the body may have widened param types (e.g. int → int?).
            if (fn_decl->name) {
                XaSymbol *fn_sym2 =
                    xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                XaSymbolLinks *fn_links2 =
                    fn_sym2 ? xa_analyzer_get_links(ctx->analyzer, fn_sym2) : NULL;
                if (fn_links2 && fn_links2->inferred_param_types) {
                    for (int pi = 0;
                         pi < fn_decl->param_count && pi < fn_links2->inferred_param_count; pi++) {
                        XrType *inferred = fn_links2->inferred_param_types[pi];
                        if (!inferred || XR_TYPE_IS_UNKNOWN(inferred))
                            continue;
                        XrParamNode *p = fn_decl->params[pi];
                        if (!p || !p->name || p->type)
                            continue;
                        XaSymbol *param_sym =
                            xa_scope_lookup_local(ctx->analyzer->current_scope, p->name);
                        if (param_sym) {
                            XaSymbolLinks *pl = xa_analyzer_get_links(ctx->analyzer, param_sym);
                            if (pl && pl->type && !xr_type_equals(pl->type, inferred)) {
                                pl->type = inferred;
                            }
                        }
                    }
                }
            }

            // Infer return type from collected returns (while still in scope)
            if (!fn_decl->return_type && fn_decl->name) {
                XrType *inferred_ret = xa_infer_compute_return_type(ctx);
                if (inferred_ret && !XR_TYPE_IS_UNKNOWN(inferred_ret) &&
                    !XR_TYPE_IS_UNIT(inferred_ret)) {
                    XaSymbol *fn_sym =
                        xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                    if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION) {
                        XaSymbolLinks *fn_links = xa_analyzer_get_links(ctx->analyzer, fn_sym);
                        if (fn_links && fn_links->type && XR_TYPE_IS_FUNCTION(fn_links->type)) {
                            fn_links->type->function.return_type = inferred_ret;
                            fn_links->return_type = inferred_ret;
                        }
                    }
                }
            }

            if (fn_decl->name) {
                XaSymbol *fn_sym =
                    xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                XaSymbolLinks *fn_links = fn_sym && fn_sym->kind == XA_SYM_FUNCTION
                                              ? xa_analyzer_get_links(ctx->analyzer, fn_sym)
                                              : NULL;
                if (fn_links) {
                    fn_links->return_storage_owner = ctx->return_storage_owner;
                    fn_links->return_storage_known = ctx->return_storage_known &&
                                                     !ctx->return_storage_mixed &&
                                                     !ctx->return_storage_unknown;
                    fn_links->return_storage_mixed =
                        ctx->return_storage_mixed ||
                        (ctx->return_storage_known && ctx->return_storage_unknown);
                    fn_links->return_storage_scanned = true;
                    fn_links->return_storage_scan_in_progress = false;
                }
                if (fn_links && fn_links->return_storage_mixed) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[320];
                    snprintf(msg, sizeof(msg),
                             "function '%s' has mixed return storage; make every return path use "
                             "the same storage owner or return copy-normalized local values",
                             fn_decl->name ? fn_decl->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }

            ctx->expected_return_type = saved_expected_ret;

            xa_analyzer_exit_scope(ctx->analyzer);

            // Restore flow state to outer function's context
            if (ctx->flow) {
                ctx->flow->current_flow = saved_flow;
                ctx->flow->current_break_target = saved_break;
                ctx->flow->current_continue_target = saved_continue;
                ctx->flow->current_return_target = saved_return;
                ctx->flow->current_exception_target = saved_exception;
            }

            // Restore outer function's return type state
            if (ctx->return_types)
                xr_free(ctx->return_types);
            ctx->return_types = saved_return_types;
            ctx->return_type_count = saved_return_count;
            ctx->return_type_capacity = saved_return_cap;
            ctx->return_storage_owner = saved_return_storage_owner;
            ctx->return_storage_known = saved_return_storage_known;
            ctx->return_storage_mixed = saved_return_storage_mixed;
            ctx->return_storage_unknown = saved_return_storage_unknown;
            break;
        }
        case AST_EXPORT_STMT:
            /* Re-exports have no local declaration body to infer. */
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL: {
            // Infer method bodies inside the class/struct. Unions have none.
            ClassDeclNode *cls = (node->type == AST_CLASS_DECL)    ? &node->as.class_decl
                                 : (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl
                                                                   : &node->as.union_decl;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
            // Establish class context so member-visibility and const-field
            // checks know which class body they are inside.
            XrClassInfo *saved_class_info = ctx->current_class_info;
            const char *saved_class_name = ctx->current_class_name;
            ctx->current_class_name = cls->name;
            ctx->current_class_info = NULL;
            {
                XaSymbol *class_sym =
                    cls->symbol_id
                        ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, cls->symbol_id)
                        : NULL;
                if (!class_sym)
                    class_sym = xa_scope_lookup(ctx->analyzer->current_scope, cls->name);
                if (!class_sym)
                    class_sym = xa_scope_lookup(ctx->analyzer->global_scope, cls->name);
                ctx->analyzer->current_scope->class_symbol = class_sym;
                if (class_sym) {
                    XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, class_sym);
                    if (cl)
                        ctx->current_class_info = cl->class_info;
                }
            }
            for (int i = 0; i < cls->method_count; i++) {
                if (cls->methods[i] && cls->methods[i]->as.method_decl.body) {
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, cls->methods[i]);
                    // Save/restore expected_return_type so the enclosing
                    // function's type doesn't leak into method bodies.
                    XrType *saved_ret = ctx->expected_return_type;
                    bool saved_is_ctor = ctx->current_method_is_constructor;
                    MethodDeclNode *md = &cls->methods[i]->as.method_decl;
                    XaSymbol *method_sym =
                        xa_scope_lookup_local(ctx->analyzer->current_scope->parent, md->name);
                    XaSymbolLinks *method_links =
                        method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
                    xa_apply_param_storage_requirements_to_scope(ctx, method_links);
                    ctx->current_method_is_constructor = md->is_constructor;
                    if (md->return_type) {
                        ctx->expected_return_type =
                            xr_tref_resolve_in_analyzer(ctx->analyzer, md->return_type);
                    } else {
                        ctx->expected_return_type = NULL;
                    }
                    xa_visit_function_body_unified(ctx, md->body);
                    xa_check_out_params_assigned_at_function_exit(ctx, ctx->analyzer->current_scope,
                                                                  md->body);
                    ctx->expected_return_type = saved_ret;
                    ctx->current_method_is_constructor = saved_is_ctor;
                    xa_analyzer_exit_scope(ctx->analyzer);
                }
            }
            ctx->current_class_info = saved_class_info;
            ctx->current_class_name = saved_class_name;
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            int out_da_count = 0;
            XaOutParamDaState *out_da = xa_out_param_da_capture(ctx, &out_da_count);
            xa_out_param_da_begin_path_merge(out_da, out_da_count);

            if (tc->try_body)
                xa_visit_infer_stmt(ctx, tc->try_body);
            xa_out_param_da_record_path(out_da, out_da_count,
                                        xa_statement_can_fall_through(tc->try_body));

            bool catch_paths_reachable = xa_out_da_try_catch_may_enter_catch(ctx, tc);
            for (int ci = 0; ci < tc->catch_count; ci++) {
                XrCatchClause *cc = tc->catch_clauses[ci];
                xa_out_param_da_restore_before(out_da, out_da_count);
                if (!cc || !cc->body) {
                    if (catch_paths_reachable)
                        xa_out_param_da_record_path(out_da, out_da_count, true);
                    continue;
                }
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, cc->body);
                if (cc->var_name) {
                    XaSymbol *err_sym = xa_symbol_new(cc->var_name, XA_SYM_VARIABLE);
                    err_sym->location.line = cc->var_line;
                    err_sym->location.column = cc->var_column;
                    xa_scope_add_symbol(ctx->analyzer->current_scope, err_sym);
                    XaSymbolLinks *err_links = xa_analyzer_get_links(ctx->analyzer, err_sym);
                    if (err_links) {
                        err_links->type = xa_resolve_catch_binding_type(ctx, cc, true);
                        err_links->is_definitely_assigned = true;
                    }
                }
                if (cc->pattern) {
                    XrType *catch_type = xa_resolve_catch_binding_type(ctx, cc, true);
                    xa_register_catch_pattern_bindings(ctx, cc, catch_type);
                }
                xa_visit_inline_statement_sequence_with_cursor(ctx, cc->body);
                xa_analyzer_exit_scope(ctx->analyzer);
                if (catch_paths_reachable)
                    xa_out_param_da_record_path(out_da, out_da_count,
                                                xa_statement_can_fall_through(cc->body));
            }
            xa_out_param_da_apply_path_merge(out_da, out_da_count);
            xa_out_param_da_free(out_da);
            break;
        }
        case AST_THROW_STMT:
            if (node->as.throw_stmt.expression) {
                XrType *thrown = xa_visit_infer_expr(ctx, node->as.throw_stmt.expression);
                if (thrown && !XR_TYPE_IS_UNKNOWN(thrown)) {
                    if (!xa_is_enum_error_type(thrown)) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "throw expression must be an enum error value; use panic channel "
                                 "for runtime faults; "
                                 "got '%s'",
                                 xr_type_to_string(thrown));
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_THROW_NON_EXCEPTION, msg, &loc);
                    }
                }
            }
            // Mark flow as unreachable after throw
            if (ctx->flow) {
                ctx->flow->current_flow = ctx->flow->unreachable_flow;
            }
            break;
        case AST_TYPE_ALIAS:
            // Type alias is compile-time concept, already registered in pass 1
            break;
        case AST_GLOBAL_ASM:
            if (!xa_freestanding_profile_enabled(ctx->analyzer)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                    "global asm is only supported in freestanding AOT profile", &loc);
            }
            break;
        case AST_FOR_IN_STMT: {
            ForInStmtNode *fi = &node->as.for_in_stmt;

            // Enter the scope created in pass 1
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

            // Infer collection type
            XrType *coll_type = NULL;
            bool is_enum_iter = false;
            const char *enum_name = NULL;

            if (fi->collection) {
                if (fi->collection->type == AST_RANGE) {
                    // Visit range sub-expressions so variables get symbol_id resolved
                    if (fi->collection->as.range.start)
                        xa_visit_infer_expr(ctx, fi->collection->as.range.start);
                    if (fi->collection->as.range.end)
                        xa_visit_infer_expr(ctx, fi->collection->as.range.end);
                    coll_type = xr_type_new_int(NULL);
                } else if (fi->collection->type == AST_VARIABLE) {
                    // Check if variable is an enum
                    XaSymbol *coll_sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                         fi->collection->as.variable.name);
                    if (coll_sym && coll_sym->kind == XA_SYM_ENUM) {
                        is_enum_iter = true;
                        // Visit the collection variable so its symbol_id is resolved
                        xa_visit_infer_expr(ctx, fi->collection);
                        XaSymbolLinks *coll_links = xa_analyzer_get_links(ctx->analyzer, coll_sym);
                        if (coll_links && coll_links->type) {
                            enum_name = coll_links->type->enum_type.enum_name;
                        }
                    } else {
                        coll_type = xa_visit_infer_expr(ctx, fi->collection);
                    }
                } else {
                    coll_type = xa_visit_infer_expr(ctx, fi->collection);
                }
            }

            /*
             * Unified for-in type inference rules:
             *   Array<T>   → item: T
             *   Span<T>    → item: T
             *   Map<K,V>   → item: Json (entry), or k: K, v: V
             *   Set<T>     → item: T
             *   Range      → item: int
             *   Channel<T> → item: T
             *   string     → item: string
             *   other      → item: unknown
             */
            XrType *item_type = xr_type_new_unknown(NULL);
            XrType *value_type = xr_type_new_unknown(NULL);

            if (is_enum_iter) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "enum '%s' is not iterable; enum case iteration requires explicit "
                         "generated metadata",
                         enum_name ? enum_name : "<unknown>");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            } else if (fi->collection && fi->collection->type == AST_RANGE) {
                item_type = xr_type_new_int(NULL);
            } else if (coll_type) {
                if (XR_TYPE_IS_ARRAY(coll_type) || XR_TYPE_IS_VIEW(coll_type) ||
                    XR_TYPE_IS_SPAN(coll_type)) {
                    if (coll_type->container.element_type) {
                        item_type = coll_type->container.element_type;
                    }
                    if (fi->is_keyvalue) {
                        value_type = item_type;
                        item_type = xr_type_new_int(NULL);  // key is index
                    }
                } else if (XR_TYPE_IS_MAP(coll_type)) {
                    if (fi->is_keyvalue) {
                        item_type = coll_type->map.key_type ? coll_type->map.key_type
                                                            : xr_type_new_unknown(NULL);
                        value_type = coll_type->map.value_type ? coll_type->map.value_type
                                                               : xr_type_new_unknown(NULL);
                    } else if (coll_type->map.key_type) {
                        item_type = coll_type->map.key_type;
                    }
                } else if (coll_type->kind == XR_KIND_SET) {
                    if (coll_type->container.element_type) {
                        item_type = coll_type->container.element_type;
                    }
                } else if (coll_type->kind == XR_KIND_CHANNEL) {
                    if (fi->is_keyvalue) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        xa_analyzer_add_diagnostic(
                            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                            "Channel iteration yields values only; use `for (msg in ch)`", &loc);
                    }
                    if (coll_type->container.element_type) {
                        item_type = coll_type->container.element_type;
                    }
                } else if (XR_TYPE_IS_STRING(coll_type)) {
                    item_type = xr_type_new_rune(NULL);
                    if (fi->is_keyvalue) {
                        value_type = xr_type_new_rune(NULL);
                        item_type = xr_type_new_int(NULL);  // key is index
                    }
                } else if (XR_TYPE_IS_JSON(coll_type)) {
                    // Json object iteration: keys are string, values are Json
                    item_type = xr_type_new_json(ctx->analyzer->isolate);
                    if (fi->is_keyvalue) {
                        value_type = xr_type_new_json(ctx->analyzer->isolate);
                        item_type = xr_type_new_string(NULL);
                    }
                } else if (XR_TYPE_IS_TUPLE(coll_type)) {
                    /* Tuples are heterogeneous by design — there is no
                     * single element type. Iteration would either widen
                     * to a useless union or reach for `any`, neither of
                     * which is acceptable in xray. The user wants .N /
                     * destructuring / pattern matching, not for-in. */
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "tuple type '%s' is not iterable; use '.0', '.1' or destructure "
                             "with 'var (a, b, ...) = t' instead",
                             xr_type_to_string(coll_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                } else {
                    /* Anything else (instance of a class / struct, json
                     * literal type, etc.) iterates only via the
                     * iterator() / hasNext() / next() protocol. If the
                     * type doesn't satisfy that contract we emit a
                     * compile-time diagnostic — the alternative is a
                     * runtime "method 'iterator' not found" which is
                     * strictly worse. */
                    XrType *elem = NULL;
                    if (xa_analyzer_is_iterable(ctx->analyzer, coll_type, &elem) && elem) {
                        item_type = elem;
                    } else {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "type '%s' is not iterable; expected an Array, Map, Set, string, "
                                 "Json, range or a class with an 'iterator()' method",
                                 xr_type_to_string(coll_type));
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    }
                }
            }

            bool collection_shared_provenance =
                fi->collection && coll_type &&
                xa_expr_yields_shared_provenance(ctx, fi->collection, coll_type);

            // Set item variable type
            if (fi->item_name) {
                XaSymbol *item_sym = xa_scope_lookup(ctx->analyzer->current_scope, fi->item_name);
                if (item_sym) {
                    if (fi->item_symbol_id == 0)
                        fi->item_symbol_id = item_sym->id;
                    item_sym->is_shared_provenance = collection_shared_provenance &&
                                                     xa_type_needs_borrow_escape_guard(item_type);
                    XaSymbolLinks *item_links = xa_analyzer_get_links(ctx->analyzer, item_sym);
                    if (item_links)
                        item_links->type = item_type;
                }
            }

            // Cache the inferred item type on the for-in node itself so the
            // IR lowerer can recover heterogeneous element types (e.g. tuple
            // elements in Array<(K,V)>) without re-running inference.
            if (item_type)
                xa_analyzer_set_node_type(ctx->analyzer, node, item_type);

            // Set value variable type (key-value mode)
            if (fi->is_keyvalue && fi->value_name) {
                XaSymbol *val_sym = xa_scope_lookup(ctx->analyzer->current_scope, fi->value_name);
                if (val_sym) {
                    if (fi->value_symbol_id == 0)
                        fi->value_symbol_id = val_sym->id;
                    val_sym->is_shared_provenance = collection_shared_provenance &&
                                                    xa_type_needs_borrow_escape_guard(value_type);
                    XaSymbolLinks *val_links = xa_analyzer_get_links(ctx->analyzer, val_sym);
                    if (val_links)
                        val_links->type = value_type;
                }
            }

            int out_da_count = 0;
            XaOutParamDaState *out_da = xa_out_param_da_capture(ctx, &out_da_count);

            // Infer body - process block statements inline (without xa_visit_block_stmt)
            // to match Pass 1 scope structure: Pass 1 processes for-in body block
            // statements in the for-in scope, so Pass 2 must do the same.
            XaLoopScope loop_scope;
            xa_loop_scope_push(ctx, &loop_scope, fi->label, node);
            if (fi->body)
                xa_visit_inline_statement_sequence_with_cursor(ctx, fi->body);
            xa_loop_scope_pop(ctx, &loop_scope);
            xa_out_param_da_restore_before(out_da, out_da_count);
            xa_out_param_da_free(out_da);

            xa_clear_active_span_borrows_in_scope(ctx, ctx->analyzer->current_scope);
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_DESTRUCTURE_DECL: {
            DestructureDeclNode *dd = &node->as.destructure_decl;
            // Infer initializer type
            XrType *init_type = dd->initializer ? xa_visit_infer_expr(ctx, dd->initializer)
                                                : xr_type_new_unknown(NULL);

            // Set types on bound variables
            if (dd->pattern) {
                XrDestructurePattern *pat = dd->pattern;
                if (pat->type == PATTERN_ARRAY) {
                    XrType *elem_type = (init_type && XR_TYPE_IS_ARRAY(init_type) &&
                                         init_type->container.element_type)
                                            ? init_type->container.element_type
                                            : xr_type_new_unknown(NULL);
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            elem->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    links->type = elem_type;
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                } else if (pat->type == PATTERN_TUPLE) {
                    /* Tuple destructuring is heterogeneous: each
                     * sub-pattern receives the type at the matching
                     * tuple position. Arity must match exactly — there
                     * is no rest pattern, so a mismatch is always a
                     * static error. Non-tuple init types still bind
                     * the variables to unknown so name-resolution in
                     * the body keeps working after the diagnostic. */
                    int pat_count = pat->as.array.element_count;
                    bool init_is_tuple = init_type && XR_TYPE_IS_TUPLE(init_type);
                    if (init_is_tuple && init_type->tuple.element_count != pat_count) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "tuple pattern has %d element(s) but value has %d", pat_count,
                                 init_type->tuple.element_count);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    } else if (init_type && !XR_TYPE_IS_TUPLE(init_type) &&
                               !XR_TYPE_IS_UNKNOWN(init_type)) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        /* Detect for-in tuple destructure context by
                         * checking for the synthesised iterator variable
                         * name prefix emitted by xr_parse_for_in_statement. */
                        bool is_for_in_ctx =
                            dd->initializer && dd->initializer->type == AST_VARIABLE &&
                            dd->initializer->as.variable.name &&
                            strncmp(dd->initializer->as.variable.name, "__for_in_tuple_", 15) == 0;
                        char msg[256];
                        if (is_for_in_ctx) {
                            snprintf(
                                msg, sizeof(msg),
                                "tuple destructuring requires a tuple value, got '%s'; "
                                "use `for (k, v in coll)` for key-value enumeration, "
                                "or `for ((k, v) in coll.entries())` to destructure entry tuples",
                                xr_type_to_string(init_type));
                        } else {
                            snprintf(msg, sizeof(msg),
                                     "tuple destructuring requires a tuple value, got '%s'",
                                     xr_type_to_string(init_type));
                        }
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    }
                    for (int i = 0; i < pat_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            elem->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    XrType *t =
                                        (init_is_tuple && i < init_type->tuple.element_count)
                                            ? init_type->tuple.element_types[i]
                                            : xr_type_new_unknown(NULL);
                                    links->type = t ? t : xr_type_new_unknown(NULL);
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                } else if (pat->type == PATTERN_OBJECT) {
                    for (int i = 0; i < pat->as.object.field_count; i++) {
                        XrDestructurePattern *vp = pat->as.object.patterns[i];
                        if (vp && vp->type == PATTERN_IDENTIFIER && vp->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            vp->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    // Try to infer field type from init_type
                                    XrType *field_type = NULL;
                                    const char *field_name = (pat->as.object.field_names &&
                                                              i < pat->as.object.field_count)
                                                                 ? pat->as.object.field_names[i]
                                                                 : vp->as.identifier.name;
                                    if (init_type && field_name) {
                                        field_type =
                                            xr_type_object_get_field(init_type, field_name);
                                    }
                                    links->type =
                                        field_type ? field_type : xr_type_new_unknown(NULL);
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                xa_visit_infer_expr(ctx, node->as.print_stmt.exprs[i]);
            }
            break;
        case AST_INDEX_SET: {
            IndexSetNode *is = &node->as.index_set;
            XrType *array_type = NULL;
            XrType *index_type = NULL;
            if (is->array)
                array_type = xa_visit_infer_expr(ctx, is->array);
            if (is->index)
                index_type = xa_visit_infer_expr(ctx, is->index);
            XrType *value_expected = NULL;
            if (array_type) {
                if ((XR_TYPE_IS_ARRAY(array_type) || XR_TYPE_IS_VIEW(array_type) ||
                     XR_TYPE_IS_SPAN(array_type)) &&
                    array_type->container.element_type) {
                    value_expected = array_type->container.element_type;
                } else if (array_type->kind == XR_KIND_FIXED_ARRAY &&
                           array_type->fixed_array.element_type) {
                    value_expected = array_type->fixed_array.element_type;
                } else if (XR_TYPE_IS_MAP(array_type) && array_type->map.value_type) {
                    value_expected = array_type->map.value_type;
                } else if (XR_TYPE_HAS_OBJECT_SHAPE(array_type)) {
                    if (array_type->object.field_count > 0 && is->index &&
                        is->index->type == AST_LITERAL_STRING) {
                        const char *key = is->index->as.literal.raw_value.string_val;
                        int field_idx = object_shape_field_index_local(array_type, key);
                        if (field_idx >= 0 && array_type->object.field_types)
                            value_expected = array_type->object.field_types[field_idx];
                    }
                    if (!value_expected && XR_TYPE_IS_JSON(array_type))
                        value_expected = xr_type_new_json(ctx->analyzer->isolate);
                }
            }
            char owner_path[512] = {0};
            XaSymbol *owner = xa_span_borrow_owner_path_for_index_write(
                ctx, is->array, is->index, value_expected, owner_path, sizeof(owner_path));
            if (owner) {
                xa_check_active_span_borrow_owner_path_mutation(ctx, node, owner,
                                                                owner_path[0] ? owner_path : NULL,
                                                                "reassigning owner element");
            }
            XrType *value_type = NULL;
            if (is->value) {
                XrType *saved_expected = ctx->expected_type;
                if (value_expected && !XR_TYPE_IS_UNKNOWN(value_expected))
                    ctx->expected_type = value_expected;
                value_type = xa_visit_infer_expr(ctx, is->value);
                ctx->expected_type = saved_expected;
            }
            xa_check_span_value_escape(ctx, node, value_type,
                                       "store Slice view into an index target");
            xa_check_pointer_borrow_escape(ctx, node, is->value, value_type,
                                           "store raw pointer borrow into an index target");
            if (xa_type_needs_borrow_escape_guard(value_type)) {
                XaSymbol *borrowed_root = xa_borrowed_param_root_symbol(ctx, is->value);
                if (borrowed_root) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    const char *mode = borrowed_root->passing_mode == XR_PARAM_REF ? "ref" : "in";
                    const char *name = borrowed_root->name ? borrowed_root->name : "?";
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot store borrowed '%s' parameter '%s' into index target; "
                             "store an owned value or copy(%s)",
                             mode, name, name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
            XaSymbol *in_param = xa_in_param_symbol_for_expr(ctx, is->array);
            if (in_param) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Cannot assign through 'in' parameter '%s' (readonly reference)",
                         in_param->name ? in_param->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
            if (array_type && (XR_TYPE_IS_SPAN(array_type) || XR_TYPE_IS_VIEW(array_type))) {
                XaSymbol *root = xa_root_variable_symbol_for_expr(ctx, is->array);
                if (root && (root->is_const || xa_symbol_has_shared_provenance(root))) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[192];
                    if (xa_symbol_has_shared_provenance(root)) {
                        snprintf(msg, sizeof(msg), "Cannot assign through shared binding '%s'",
                                 root->name ? root->name : "?");
                    } else {
                        snprintf(msg, sizeof(msg), "Cannot assign through const view '%s'",
                                 root->name ? root->name : "?");
                    }
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                }
            }
            if (array_type && !(XR_TYPE_IS_SPAN(array_type) || XR_TYPE_IS_VIEW(array_type))) {
                XaSymbol *root = xa_root_variable_symbol_for_expr(ctx, is->array);
                bool readonly_array = xr_type_is_const(array_type);
                if ((root &&
                     (root->is_readonly_binding || xa_symbol_has_shared_provenance(root))) ||
                    readonly_array) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[192];
                    const char *label = root && xa_symbol_has_shared_provenance(root)
                                            ? "shared binding"
                                        : root && root->is_readonly_binding ? "const binding"
                                                                            : "readonly value";
                    snprintf(msg, sizeof(msg), "Cannot assign through %s '%s'", label,
                             root && root->name ? root->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                }
            }
            // Tuples are immutable: reject index-based assignment
            if (array_type && XR_TYPE_IS_TUPLE(array_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TUPLE_IMMUTABLE,
                    "Cannot assign to tuple element: tuples are immutable", &loc);
                break;
            }
            // FFI raw pointer store p[i] = v: writes *(p + i). Unsafe (no bounds/
            // null check), and only a mutable MutPtr<T> may be written through.
            if (array_type && XR_TYPE_IS_POINTER(array_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                if (ctx->unsafe_depth == 0) {
                    xa_analyzer_add_diagnostic(
                        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                        "raw pointer store `p[i] = v` must be inside an `unsafe { }` block", &loc);
                }
                if (!array_type->ptr_is_mut) {
                    xa_analyzer_add_diagnostic(
                        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                        "cannot store through a const `Ptr<T>` (use `MutPtr<T>`)", &loc);
                }
                if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type)) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "Index type '%s' is not assignable to expected type 'int'",
                             xr_type_to_string(index_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
                break;
            }
            if (array_type && XR_TYPE_HAS_OBJECT_SHAPE(array_type) &&
                array_type->object.field_count > 0 && is->index &&
                is->index->type == AST_LITERAL_STRING) {
                const char *key = is->index->as.literal.raw_value.string_val;
                int field_idx = object_shape_field_index_local(array_type, key);
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                if (field_idx >= 0) {
                    bool readonly = array_type->object.field_readonly
                                        ? array_type->object.field_readonly[field_idx]
                                        : false;
                    if (readonly) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "无法修改只读字段 '%s.%s'（const 修饰）",
                                 object_shape_type_label_local(array_type), key);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                    }
                } else if (array_type->object.is_sealed) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "类型 '%s' 不允许添加字段 '%s'",
                             object_shape_type_label_local(array_type), key);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            } else if (array_type && XR_TYPE_IS_RECORD(array_type) &&
                       array_type->object.is_sealed) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH,
                                           "sealed Record index assignment requires a string "
                                           "literal key",
                                           &loc);
            } else if (array_type &&
                       (XR_TYPE_IS_ARRAY(array_type) || XR_TYPE_IS_VIEW(array_type) ||
                        XR_TYPE_IS_SPAN(array_type) || array_type->kind == XR_KIND_FIXED_ARRAY)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Index type '%s' is not assignable to expected type 'int'",
                             xr_type_to_string(index_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
                XrType *elem_type = NULL;
                if ((XR_TYPE_IS_ARRAY(array_type) || XR_TYPE_IS_VIEW(array_type) ||
                     XR_TYPE_IS_SPAN(array_type)) &&
                    array_type->container.element_type) {
                    elem_type = array_type->container.element_type;
                } else if (array_type->kind == XR_KIND_FIXED_ARRAY) {
                    elem_type = array_type->fixed_array.element_type;
                }
                if (elem_type && value_type && !XR_TYPE_IS_UNKNOWN(value_type)) {
                    xa_analyzer_check_assignment(ctx->analyzer, elem_type, value_type, &loc);
                }
            } else if (array_type && XR_TYPE_IS_STRING(array_type) && index_type &&
                       !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Index type '%s' is not assignable to expected type 'int'",
                         xr_type_to_string(index_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
            break;
        }
        case AST_DEFER_STMT:
            xa_freestanding_report_unavailable(ctx, node, "defer statement",
                                               "defer uses hosted cleanup/runtime state");
            if (node->as.defer_stmt.expr)
                xa_visit_infer_expr(ctx, node->as.defer_stmt.expr);
            break;
        case AST_SCOPE_BLOCK:
            if (node->as.scope_block.body)
                xa_visit_infer_stmt(ctx, node->as.scope_block.body);
            break;
        case AST_SELECT_STMT: {
            SelectStmtNode *sel = &node->as.select_stmt;
            for (int i = 0; i < sel->case_count; i++) {
                if (sel->cases[i])
                    xa_visit_infer_stmt(ctx, sel->cases[i]);
            }
            break;
        }
        case AST_SELECT_CASE: {
            SelectCaseNode *sc = &node->as.select_case;
            if (sc->channel)
                xa_visit_infer_expr(ctx, sc->channel);
            if (sc->value) {
                xa_visit_infer_expr(ctx, sc->value);
                /* Send arms cross a coroutine boundary just like ch.send / go,
                 * so a bare owned-heap payload must use explicit copy/move/
                 * shared. */
                if (sc->is_send) {
                    XrType *send_type = xa_analyzer_get_node_type(ctx->analyzer, sc->value);
                    if (!send_type)
                        send_type = xa_visit_infer_expr(ctx, sc->value);
                    xa_check_boundary_transfer_arg(ctx, node, sc->value, send_type,
                                                   "select send value");
                }
            }
            /* Recv cases have a block scope from pass 1 for the variable. */
            if (sc->var_name && !sc->is_send && !sc->is_default) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
                if (sc->body) {
                    if (sc->body->type == AST_BLOCK) {
                        BlockNode *blk = &sc->body->as.block;
                        for (int si = 0; si < blk->count; si++)
                            xa_visit_infer_stmt(ctx, blk->statements[si]);
                    } else {
                        xa_visit_infer_stmt(ctx, sc->body);
                    }
                }
                xa_analyzer_exit_scope(ctx->analyzer);
            } else {
                if (sc->body)
                    xa_visit_infer_stmt(ctx, sc->body);
            }
            break;
        }
        case AST_DESTRUCTURE_ASSIGN: {
            DestructureAssignNode *da = &node->as.destructure_assign;
            if (da->value)
                xa_visit_infer_expr(ctx, da->value);
            /* Resolve symbol_ids on pattern target identifiers so the
             * lowerer can find existing variables via Braun SSA. */
            if (da->pattern) {
                XrDestructurePattern *pat = da->pattern;
                if (pat->type == PATTERN_ARRAY || pat->type == PATTERN_TUPLE) {
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            elem->as.identifier.name);
                            if (sym)
                                elem->as.identifier.symbol_id = sym->id;
                        }
                    }
                } else if (pat->type == PATTERN_OBJECT) {
                    for (int i = 0; i < pat->as.object.field_count; i++) {
                        XrDestructurePattern *vp = pat->as.object.patterns[i];
                        if (vp && vp->type == PATTERN_IDENTIFIER && vp->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            vp->as.identifier.name);
                            if (sym)
                                vp->as.identifier.symbol_id = sym->id;
                        }
                    }
                }
            }
            break;
        }
        case AST_BREAK_STMT:
            xa_validate_loop_control(ctx, node, node->as.break_stmt.label, false);
            break;
        case AST_CONTINUE_STMT:
            xa_validate_loop_control(ctx, node, node->as.continue_stmt.label, true);
            break;
        case AST_ENUM_DECL: {
            EnumDeclNode *ed = &node->as.enum_decl;
            XaSymbol *enum_sym =
                ed->name ? xa_scope_lookup(ctx->analyzer->current_scope, ed->name) : NULL;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
            if (enum_sym && enum_sym->kind == XA_SYM_ENUM)
                ctx->analyzer->current_scope->class_symbol = enum_sym;
            for (int i = 0; i < ed->method_count; i++) {
                AstNode *method = ed->methods ? ed->methods[i] : NULL;
                if (!method || method->type != AST_METHOD_DECL || !method->as.method_decl.body)
                    continue;
                MethodDeclNode *md = &method->as.method_decl;
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, method);
                XaSymbol *method_sym =
                    xa_scope_lookup_local(ctx->analyzer->current_scope->parent, md->name);
                XaSymbolLinks *method_links =
                    method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
                xa_apply_param_storage_requirements_to_scope(ctx, method_links);
                XrType *saved_ret = ctx->expected_return_type;
                ctx->expected_return_type =
                    md->return_type ? xr_tref_resolve_in_analyzer(ctx->analyzer, md->return_type)
                                    : NULL;
                xa_visit_function_body_unified(ctx, md->body);
                xa_check_out_params_assigned_at_function_exit(ctx, ctx->analyzer->current_scope,
                                                              md->body);
                ctx->expected_return_type = saved_ret;
                xa_analyzer_exit_scope(ctx->analyzer);
            }
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_YIELD_STMT: {
            /* `yield expr` produces a generator value. The enclosing function
             * must be a generator declared `-> Iterator<T>`; the yielded value
             * must be assignable to T. Mark the function as a generator so IR
             * lowering emits the generator entry. */
            YieldStmtNode *ys = &node->as.yield_stmt;
            XrType *val_type = ys->value ? xa_visit_infer_expr(ctx, ys->value) : NULL;
            ctx->current_fn_has_yield = true;
            XrType *elem = NULL;
            XrLocation yloc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            bool is_gen_ret =
                xa_generator_return_element(ctx->analyzer, ctx->expected_return_type, &elem);
            if (!is_gen_ret) {
                /* `yield expr` requires an enclosing function declared
                 * `-> Iterator<T>` (covers top-level yield and yield inside a
                 * non-generator function). Single diagnostic for both. */
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                    "`yield` is only valid inside a generator function declared `-> Iterator<T>`",
                    &yloc);
            } else if (elem && val_type && !xa_typecheck_assignable(elem, val_type)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "yielded value of type '%s' is not assignable to generator element "
                         "type '%s'",
                         xr_type_to_string(val_type), xr_type_to_string(elem));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &yloc);
            } else if (ys->value && val_type) {
                xa_check_borrowed_yield_escape(ctx, node, ys->value, val_type);
                xa_check_span_value_escape(ctx, ys->value, val_type,
                                           "yield Slice view from generator");
            }
            break;
        }
        case AST_IMPORT_STMT:
        case AST_INTERFACE_DECL:
            break;
        default:
            /* Expressions used as statements (go, await, call, etc.)
             * fall through to xa_visit_infer_expr. */
            xa_visit_infer_expr(ctx, node);
            break;
    }
}

static void xa_collect_error_sources_expr(XaAnalyzer *analyzer, AstNode *node,
                                          XaEffectSummary *out);
static void xa_collect_error_sources_stmt(XaAnalyzer *analyzer, AstNode *node,
                                          XaEffectSummary *out);

static XR_THREAD_LOCAL AstNode *xa_payload_error_scan_root = NULL;
static XR_THREAD_LOCAL int xa_payload_error_scan_depth = 0;

static AstNode *xa_find_function_decl_by_name(AstNode *node, const char *name) {
    if (!node || !name)
        return NULL;
    if (node->type == AST_FUNCTION_DECL && node->as.function_decl.name &&
        strcmp(node->as.function_decl.name, name) == 0)
        return node;
    if (node->type == AST_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++) {
            AstNode *found = xa_find_function_decl_by_name(node->as.program.statements[i], name);
            if (found)
                return found;
        }
    }
    return NULL;
}

static void xa_payload_effect_add_node_enum_type(XaAnalyzer *analyzer, XaEffectSummary *out,
                                                 AstNode *node) {
    if (!analyzer || !out || !node)
        return;
    XrType *type = xa_analyzer_get_node_type(analyzer, node);
    if (xa_is_enum_error_type(type)) {
        XaErrorTypeId type_id = xa_effect_db_register_error_enum(analyzer->effect_db, type);
        if (type_id != XA_ERROR_TYPE_NONE)
            xa_effect_summary_add_all_variants(analyzer->effect_db, out, type_id);
    }
}

static XaSymbol *xa_payload_effect_resolve_call_target(XaAnalyzer *analyzer, AstNode *callee) {
    if (!analyzer || !callee || callee->type != AST_VARIABLE || !callee->as.variable.name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(analyzer, callee->as.variable.name);
    if (!sym)
        sym =
            xa_analyzer_lookup_in_scope(analyzer, callee->as.variable.name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, callee->as.variable.name);
    return sym;
}

static void xa_payload_effect_union_call_errors(XaAnalyzer *analyzer, XaEffectSummary *out,
                                                AstNode *callee) {
    if (!analyzer || !out || !callee)
        return;
    bool used_effect_summary = false;
    XaSymbol *callee_sym = xa_payload_effect_resolve_call_target(analyzer, callee);
    if (callee_sym && callee_sym->links.effect_id != XA_EFFECT_NONE) {
        const XaEffectSummary *callee_summary =
            xa_effect_db_get(analyzer->effect_db, callee_sym->links.effect_id);
        if (callee_summary) {
            xa_effect_summary_add_summary(analyzer->effect_db, out, callee_summary);
            used_effect_summary = true;
        }
    }
    if (callee->type == AST_VARIABLE && callee->as.variable.name) {
        if (!used_effect_summary && xa_payload_error_scan_root && xa_payload_error_scan_depth < 8) {
            AstNode *fn =
                xa_find_function_decl_by_name(xa_payload_error_scan_root, callee->as.variable.name);
            if (fn && fn->as.function_decl.body) {
                xa_payload_error_scan_depth++;
                xa_collect_error_sources_stmt(analyzer, fn->as.function_decl.body, out);
                xa_payload_error_scan_depth--;
            }
        }
    }
}

static void xa_collect_error_sources_expr_list(XaAnalyzer *analyzer, AstNode **nodes, int count,
                                               XaEffectSummary *out) {
    for (int i = 0; i < count; i++)
        xa_collect_error_sources_expr(analyzer, nodes ? nodes[i] : NULL, out);
}

static void xa_collect_error_sources_expr(XaAnalyzer *analyzer, AstNode *node,
                                          XaEffectSummary *out) {
    if (!analyzer || !node || !out)
        return;

    switch (node->type) {
        case AST_CALL_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.call_expr.callee, out);
            xa_collect_error_sources_expr_list(analyzer, node->as.call_expr.arguments,
                                               node->as.call_expr.arg_count, out);
            xa_payload_effect_union_call_errors(analyzer, out, node->as.call_expr.callee);
            break;
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
        case AST_NULLISH_COALESCE:
            xa_collect_error_sources_expr(analyzer, node->as.binary.left, out);
            xa_collect_error_sources_expr(analyzer, node->as.binary.right, out);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_collect_error_sources_expr(analyzer, node->as.unary.operand, out);
            break;
        case AST_TERNARY:
            xa_collect_error_sources_expr(analyzer, node->as.ternary.condition, out);
            xa_collect_error_sources_expr(analyzer, node->as.ternary.true_expr, out);
            xa_collect_error_sources_expr(analyzer, node->as.ternary.false_expr, out);
            break;
        case AST_GROUPING:
            xa_collect_error_sources_expr(analyzer, node->as.grouping, out);
            break;
        case AST_ASSIGNMENT:
            xa_collect_error_sources_expr(analyzer, node->as.assignment.value, out);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            xa_collect_error_sources_expr(analyzer, node->as.compound_assignment.object, out);
            xa_collect_error_sources_expr(analyzer, node->as.compound_assignment.value, out);
            break;
        case AST_DESTRUCTURE_ASSIGN:
            xa_collect_error_sources_expr(analyzer, node->as.destructure_assign.value, out);
            break;
        case AST_MEMBER_ACCESS:
            xa_collect_error_sources_expr(analyzer, node->as.member_access.object, out);
            break;
        case AST_MEMBER_SET:
            xa_collect_error_sources_expr(analyzer, node->as.member_set.object, out);
            xa_collect_error_sources_expr(analyzer, node->as.member_set.value, out);
            break;
        case AST_INDEX_GET:
            xa_collect_error_sources_expr(analyzer, node->as.index_get.array, out);
            xa_collect_error_sources_expr(analyzer, node->as.index_get.index, out);
            break;
        case AST_INDEX_SET:
            xa_collect_error_sources_expr(analyzer, node->as.index_set.array, out);
            xa_collect_error_sources_expr(analyzer, node->as.index_set.index, out);
            xa_collect_error_sources_expr(analyzer, node->as.index_set.value, out);
            break;
        case AST_SLICE_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.slice_expr.source, out);
            xa_collect_error_sources_expr(analyzer, node->as.slice_expr.start, out);
            xa_collect_error_sources_expr(analyzer, node->as.slice_expr.end, out);
            break;
        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                xa_collect_error_sources_expr(analyzer, node->as.array_literal.repeat_value, out);
                xa_collect_error_sources_expr(analyzer, node->as.array_literal.repeat_count, out);
            } else {
                xa_collect_error_sources_expr_list(analyzer, node->as.array_literal.elements,
                                                   node->as.array_literal.count, out);
            }
            break;
        case AST_TUPLE_LITERAL:
            xa_collect_error_sources_expr_list(analyzer, node->as.tuple_literal.elements,
                                               node->as.tuple_literal.count, out);
            break;
        case AST_SPREAD_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.spread_expr.expr, out);
            break;
        case AST_OBJECT_LITERAL:
            xa_collect_error_sources_expr_list(analyzer, node->as.object_literal.keys,
                                               node->as.object_literal.count, out);
            xa_collect_error_sources_expr_list(analyzer, node->as.object_literal.values,
                                               node->as.object_literal.count, out);
            break;
        case AST_MAP_LITERAL:
            xa_collect_error_sources_expr_list(analyzer, node->as.map_literal.keys,
                                               node->as.map_literal.count, out);
            xa_collect_error_sources_expr_list(analyzer, node->as.map_literal.values,
                                               node->as.map_literal.count, out);
            break;
        case AST_SET_LITERAL:
            xa_collect_error_sources_expr_list(analyzer, node->as.set_literal.elements,
                                               node->as.set_literal.count, out);
            break;
        case AST_STRUCT_LITERAL:
            xa_collect_error_sources_expr_list(analyzer, node->as.struct_literal.field_values,
                                               node->as.struct_literal.field_count, out);
            break;
        case AST_TEMPLATE_STRING:
            xa_collect_error_sources_expr_list(analyzer, node->as.template_str.parts,
                                               node->as.template_str.part_count, out);
            break;
        case AST_MATCH_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.match_expr.expr, out);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                AstNode *arm = node->as.match_expr.arms[i];
                if (arm) {
                    xa_collect_error_sources_expr(analyzer, arm->as.match_arm.guard, out);
                    xa_collect_error_sources_expr(analyzer, arm->as.match_arm.body, out);
                }
            }
            break;
        case AST_AS_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.as_expr.expr, out);
            break;
        case AST_IS_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.is_expr.expr, out);
            break;
        case AST_COMPTIME_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.comptime_expr.expr, out);
            break;
        case AST_UNSAFE_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.unsafe_expr.operand, out);
            break;
        case AST_MOVE_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.move_expr.expr, out);
            break;
        case AST_AWAIT_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.await_expr.expr, out);
            break;
        case AST_GO_EXPR:
            xa_collect_error_sources_expr(analyzer, node->as.go_expr.expr, out);
            break;
        default:
            break;
    }
}

static void xa_collect_error_sources_stmt(XaAnalyzer *analyzer, AstNode *node,
                                          XaEffectSummary *out) {
    if (!analyzer || !node || !out)
        return;

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                xa_collect_error_sources_stmt(analyzer, node->as.block.statements[i], out);
            break;
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                xa_collect_error_sources_stmt(analyzer, node->as.program.statements[i], out);
            break;
        case AST_EXPR_STMT:
            xa_collect_error_sources_expr(analyzer, node->as.expr_stmt, out);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            xa_collect_error_sources_expr(analyzer, node->as.var_decl.initializer, out);
            break;
        case AST_DESTRUCTURE_DECL:
            xa_collect_error_sources_expr(analyzer, node->as.destructure_decl.initializer, out);
            break;
        case AST_ASSIGNMENT:
            xa_collect_error_sources_expr(analyzer, node->as.assignment.value, out);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            xa_collect_error_sources_expr(analyzer, node->as.compound_assignment.object, out);
            xa_collect_error_sources_expr(analyzer, node->as.compound_assignment.value, out);
            break;
        case AST_DESTRUCTURE_ASSIGN:
            xa_collect_error_sources_expr(analyzer, node->as.destructure_assign.value, out);
            break;
        case AST_RETURN_STMT:
            xa_collect_error_sources_expr_list(analyzer, node->as.return_stmt.values,
                                               node->as.return_stmt.value_count, out);
            break;
        case AST_THROW_STMT:
            xa_collect_error_sources_expr(analyzer, node->as.throw_stmt.expression, out);
            xa_payload_effect_add_node_enum_type(analyzer, out, node->as.throw_stmt.expression);
            break;
        case AST_IF_STMT:
            xa_collect_error_sources_expr(analyzer, node->as.if_stmt.condition, out);
            xa_collect_error_sources_stmt(analyzer, node->as.if_stmt.then_branch, out);
            xa_collect_error_sources_stmt(analyzer, node->as.if_stmt.else_branch, out);
            break;
        case AST_WHILE_STMT:
            xa_collect_error_sources_expr(analyzer, node->as.while_stmt.condition, out);
            xa_collect_error_sources_stmt(analyzer, node->as.while_stmt.body, out);
            break;
        case AST_FOR_STMT:
            xa_collect_error_sources_stmt(analyzer, node->as.for_stmt.initializer, out);
            xa_collect_error_sources_expr(analyzer, node->as.for_stmt.condition, out);
            xa_collect_error_sources_expr(analyzer, node->as.for_stmt.increment, out);
            xa_collect_error_sources_stmt(analyzer, node->as.for_stmt.body, out);
            break;
        case AST_FOR_IN_STMT:
            xa_collect_error_sources_expr(analyzer, node->as.for_in_stmt.collection, out);
            xa_collect_error_sources_stmt(analyzer, node->as.for_in_stmt.body, out);
            break;
        case AST_TRY_CATCH:
            xa_collect_error_sources_stmt(analyzer, node->as.try_catch.try_body, out);
            for (int i = 0; i < node->as.try_catch.catch_count; i++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[i];
                xa_collect_error_sources_stmt(analyzer, cc ? cc->body : NULL, out);
            }
            break;
        default:
            xa_collect_error_sources_expr(analyzer, node, out);
            break;
    }
}

static XrType *xa_effect_summary_payload_enum_type(XaAnalyzer *analyzer,
                                                   const XaEffectSummary *summary,
                                                   bool *has_unsupported_mix) {
    XrType *payload_type = NULL;
    int payload_count = 0;
    bool other_error = false;

    if (has_unsupported_mix)
        *has_unsupported_mix = false;
    if (!analyzer || !summary)
        return NULL;

    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        XrType *type =
            xa_effect_db_error_type_handle(analyzer->effect_db, summary->escaping.types[i].type_id);
        if (!type) {
            other_error = true;
            continue;
        }
        if (xa_enum_error_type_has_payload(analyzer, type)) {
            payload_count++;
            if (!payload_type)
                payload_type = type;
            else if (!xr_type_equals(payload_type, type))
                other_error = true;
        } else {
            other_error = true;
        }
    }

    if (has_unsupported_mix)
        *has_unsupported_mix = other_error || payload_count > 1;
    return payload_type;
}

static XrCatchClause *xa_single_error_catch_clause(TryCatchNode *tc) {
    XrCatchClause *only = NULL;
    int count = 0;
    if (!tc)
        return NULL;
    for (int i = 0; i < tc->catch_count; i++) {
        XrCatchClause *cc = tc->catch_clauses[i];
        if (!cc || cc->is_panic)
            continue;
        only = cc;
        count++;
    }
    return count == 1 ? only : NULL;
}

static void xa_report_freestanding_payload_catch_boundary(XaAnalyzer *analyzer, XrCatchClause *cc,
                                                          XrType *payload_type, bool mixed_errors) {
    if (!analyzer || !cc)
        return;
    XrLocation loc = {.file = analyzer->current_file,
                      .line = cc->var_line > 0 ? cc->var_line : 0,
                      .column = cc->var_column > 0 ? cc->var_column : 0};
    char msg[384];
    snprintf(msg, sizeof(msg),
             "freestanding profile requires a single typed catch for payload enum error %s; "
             "%s",
             payload_type ? xr_type_to_string(payload_type) : "<enum>",
             mixed_errors ? "mixed error sets still need explicit typed lowering"
                          : "write catch (e: ThatEnum) so the no-box payload channel can be used");
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg, &loc);
}

static void xa_validate_freestanding_payload_error_catches_node(XaAnalyzer *analyzer,
                                                                AstNode *node) {
    if (!analyzer || !node)
        return;

    if (node->type == AST_TRY_CATCH) {
        TryCatchNode *tc = &node->as.try_catch;
        XaEffectSummary errors;
        xa_effect_summary_init(&errors);
        bool mixed_errors = false;
        XrType *payload_type = NULL;
        xa_collect_error_sources_stmt(analyzer, tc->try_body, &errors);
        payload_type = xa_effect_summary_payload_enum_type(analyzer, &errors, &mixed_errors);
        if (payload_type) {
            XrCatchClause *only = xa_single_error_catch_clause(tc);
            XrType *catch_type =
                (only && only->type) ? xr_tref_resolve_in_analyzer(analyzer, only->type) : NULL;
            if (!only || !catch_type || !xr_type_equals(catch_type, payload_type) || mixed_errors)
                xa_report_freestanding_payload_catch_boundary(
                    analyzer, only ? only : tc->catch_clauses[0], payload_type, mixed_errors);
        }
        xa_effect_summary_clear(&errors);

        xa_validate_freestanding_payload_error_catches_node(analyzer, tc->try_body);
        for (int i = 0; i < tc->catch_count; i++) {
            XrCatchClause *cc = tc->catch_clauses[i];
            xa_validate_freestanding_payload_error_catches_node(analyzer, cc ? cc->body : NULL);
        }
        return;
    }

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                    node->as.block.statements[i]);
            break;
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                    node->as.program.statements[i]);
            break;
        case AST_IF_STMT:
            xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                node->as.if_stmt.then_branch);
            xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            xa_validate_freestanding_payload_error_catches_node(analyzer, node->as.while_stmt.body);
            break;
        case AST_FOR_STMT:
            xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                node->as.for_stmt.initializer);
            xa_validate_freestanding_payload_error_catches_node(analyzer, node->as.for_stmt.body);
            break;
        case AST_FOR_IN_STMT:
            xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                node->as.for_in_stmt.body);
            break;
        case AST_FUNCTION_DECL:
            xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                node->as.function_decl.body);
            break;
        case AST_METHOD_DECL:
            xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                node->as.method_decl.body);
            break;
        case AST_CLASS_DECL:
            for (int i = 0; i < node->as.class_decl.method_count; i++)
                xa_validate_freestanding_payload_error_catches_node(analyzer,
                                                                    node->as.class_decl.methods[i]);
            break;
        case AST_STRUCT_DECL:
            for (int i = 0; i < node->as.struct_decl.method_count; i++)
                xa_validate_freestanding_payload_error_catches_node(
                    analyzer, node->as.struct_decl.methods[i]);
            break;
        case AST_EXPORT_STMT:
            /* Re-exports contain no executable subtree. */
            break;
        default:
            break;
    }
}

static void xa_validate_freestanding_payload_error_catches(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !ast || !xa_freestanding_profile_enabled(analyzer))
        return;
    xa_payload_error_scan_root = ast;
    xa_payload_error_scan_depth = 0;
    xa_validate_freestanding_payload_error_catches_node(analyzer, ast);
    xa_payload_error_scan_root = NULL;
    xa_payload_error_scan_depth = 0;
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================
 * xa_analyze_ast() runs the full analysis pipeline:
 *   Pass 1   -> Symbol collection
 *   Pass 1.5 -> Class inheritance linking
 *   Pass 2   -> Type inference and checking
 *   Pass 3   -> Error set inference (value-return error system)
 *   Pass 4   -> Allocation effect inference and @no_alloc validation
 *   Pass 5   -> Suspend effect validation (@no_suspend / @interrupt / @c_export)
 * ========================================================================== */

void xa_analyze_ast(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !ast)
        return;

    XaInferContext *ctx = xa_infer_context_new(analyzer);
    if (!ctx)
        return;

    xa_reset_scope_move_states(analyzer->global_scope);

    // Pass 1: Collect all symbols
    xa_visit_collect(ctx, ast);

    // Pass 1.5: Link class inheritance chains
    xa_link_class_inheritance(analyzer);

    // Pass 1.5b: Recompute receiver mutation flags after inheritance is linked.
    while (xa_propagate_receiver_mutations_for_ast(analyzer, ast)) {
    }

    // Pass 1.5c: Recompute parameter escape summaries after classes and wrappers are linked.
    while (xa_propagate_param_escape_summaries_for_ast(ctx, ast)) {
    }

    // Pass 1.6: Detect classes that form reference cycles (cycle collector)
    xa_mark_cycle_candidates(analyzer);

    // Pass 2: Infer types
    xa_visit_infer(ctx, ast);

    // Pass 3: Infer error sets for functions (value-return error system)
    xa_infer_error_sets(analyzer, ast);

    // Pass 4: Infer allocation effects from the typed, symbol-resolved AST.
    // This runs for check, VM and AOT; backends only consume the result.
    xa_infer_allocation_effects(analyzer, ast);

    // Pass 5: Validate @no_suspend assertions (and the implicit @interrupt /
    // @c_export boundaries) against the task-212 suspend effect. Fail-closed.
    xa_verify_no_suspend(analyzer, ast);

    xa_validate_freestanding_payload_error_catches(analyzer, ast);

    xa_infer_context_free(ctx);
}
