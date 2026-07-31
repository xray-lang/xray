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
#include "xa_parallel_call_plan.h"
#include "xa_resolved_call.h"
#include "xa_intrinsic_registry.h"
#include "xbuiltin_receiver_registry.h"
#include "xa_selection.h"
#include "xaddressability.h"
#include "xtype_ref_resolve.h"
#include "../parser/xtype_ref.h"
#include "xanalyzer_mono.h"
#include "xanalyzer_allocation.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xnative_package.h"
#include "../../base/xchecks.h"
#include "../../base/xconstants.h"
#include "../../base/xfileio.h"
#include "../../base/xhashmap.h"
#include "../../shared/xr_array_core.h"
#include <inttypes.h>

static bool xa_class_info_is_parallel_plan(XaInferContext *ctx, XrClassInfo *info);
static bool xa_intrinsic_is_parallel_plan_method(XaIntrinsicId intrinsic_id);
static XrType *xa_visit_call_arg_with_parallel_context(XaInferContext *ctx, AstNode *arg_node,
                                                       const char *callback_label);
static XrType *xa_visit_call_arg_for_param_mode(XaInferContext *ctx, AstNode *arg_node,
                                                const char *callback_label, XrCallArgAccess access,
                                                XrParamMode param_mode);
static bool xa_call_has_explicit_arg_access(const CallExprNode *call);
static void xa_report_arg_accesses_require_known_contract(XaInferContext *ctx, AstNode *call_node,
                                                          const CallExprNode *call);
static void xa_report_arg_access_without_matching_contract(XaInferContext *ctx, AstNode *call_node,
                                                           AstNode *arg_node,
                                                           XrCallArgAccess access, int slot);
static void xa_check_call_arg_access_authorization(XaInferContext *ctx, AstNode *call_node,
                                                   const CallExprNode *call, AstNode *arg_node,
                                                   int arg_index, int slot, XrParamMode param_mode);
static XrCallArgAccess xa_call_arg_access(const CallExprNode *call, int index);
static void xa_check_ref_argument_not_readonly(XaInferContext *ctx, AstNode *call_node,
                                               AstNode *arg_node, int slot, XrParamMode mode);
static bool xa_class_name_matches_mono_base(const char *class_name, const char *base);
static bool xa_call_object_is_module(XaInferContext *ctx, AstNode *object, const char *module_name);
static const char *xa_call_object_module_name(XaInferContext *ctx, AstNode *object);
static bool xa_type_layout_has_flexible_tail(const XrType *type);

static XaSymbolLinks *xa_refresh_imported_symbol_metadata(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !ctx->analyzer || !sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!sym->is_imported || !links || links->intrinsic_id != XA_INTRINSIC_NONE ||
        !links->module_name || !links->import_member_name)
        return links;

    const char *module_name = links->module_name;
    const char *member_name = links->import_member_name;
    bool is_quoted = module_name[0] == '.' || module_name[0] == '/';
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, module_name, is_quoted);
    XaSymbol *export_sym = exports ? (XaSymbol *) xr_hashmap_get(exports, member_name) : NULL;
    if (!export_sym && !is_quoted) {
        char key[192];
        int key_len = snprintf(key, sizeof(key), "%s.%s", module_name, member_name);
        const XaIntrinsicDesc *desc =
            key_len > 0 && (size_t) key_len < sizeof(key) ? xa_intrinsic_by_key(key) : NULL;
        if (desc)
            links->intrinsic_id = desc->id;
        return links;
    }
    if (!export_sym || export_sym->links.intrinsic_id == XA_INTRINSIC_NONE)
        return links;

    xa_symbol_links_copy_export_metadata(ctx->analyzer, links, &export_sym->links);
    links->module_name = module_name;
    links->import_member_name = member_name;
    return links;
}

static bool xa_simd_shuffle_diag_exists(const XaAnalyzer *analyzer, const XrLocation *loc) {
    if (!analyzer || !loc)
        return false;
    for (const XaDiagnostic *diag = analyzer->diagnostics; diag; diag = diag->next) {
        if (diag->code != XR_ERR_ANALYZE_ARG_TYPE || !diag->message ||
            strncmp(diag->message, "simd.", 5) != 0 || diag->location.line != loc->line ||
            diag->location.column != loc->column)
            continue;
        if ((!diag->location.file && !loc->file) ||
            (diag->location.file && loc->file && strcmp(diag->location.file, loc->file) == 0))
            return true;
    }
    return false;
}

static void xa_check_intrinsic_shuffle_lanes(XaInferContext *ctx, const AstNode *call_node,
                                             const CallExprNode *call,
                                             const XaIntrinsicDesc *desc) {
    if (!ctx || !ctx->analyzer || !call_node || !call || !desc ||
        desc->safety != XA_INTRINSIC_SAFETY_CONST_LANES ||
        (desc->flags & XA_INTRINSIC_FLAG_EXPLICIT_SHUFFLE) == 0)
        return;
    int lanes = desc->shape_rule.input_lanes;
    if (lanes <= 0)
        return;
    for (int i = 0; i < call->arg_count; i++) {
        int64_t lane = -1;
        const char *consteval_error = NULL;
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        if (!arg || !xa_consteval_int_expr(ctx->analyzer, arg, &lane, &consteval_error)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = arg ? arg->line : call_node->line,
                              .column = arg ? arg->column : call_node->column};
            char msg[224];
            snprintf(msg, sizeof(msg), "%s lane %d must be a compile-time integer", desc->key,
                     i + 1);
            if (!xa_simd_shuffle_diag_exists(ctx->analyzer, &loc))
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
            continue;
        }
        if (lane < 0 || lane >= lanes) {
            XrLocation loc = {.file = ctx->file_path, .line = arg->line, .column = arg->column};
            char msg[224];
            snprintf(msg, sizeof(msg),
                     "%s lane %d is out of range: got %" PRId64 ", expected 0..%d", desc->key,
                     i + 1, lane, lanes - 1);
            if (!xa_simd_shuffle_diag_exists(ctx->analyzer, &loc))
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
        }
    }
}

static XaIntrinsicId xa_builtin_receiver_method_intrinsic_id(XaBuiltinReceiverMethodId method_id) {
    switch (method_id) {
        case XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_POPCOUNT:
            return XA_INTRINSIC_BITS_POPCOUNT;
        case XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_LEADING_ZEROS:
            return XA_INTRINSIC_BITS_LEADING_ZEROS;
        case XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_TRAILING_ZEROS:
            return XA_INTRINSIC_BITS_TRAILING_ZEROS;
        case XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_BYTESWAP:
            return XA_INTRINSIC_BITS_BYTESWAP;
        case XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_ROTATE_LEFT:
            return XA_INTRINSIC_BITS_ROTATE_LEFT;
        case XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_ROTATE_RIGHT:
            return XA_INTRINSIC_BITS_ROTATE_RIGHT;
        case XA_BUILTIN_RECEIVER_METHOD_EXACT_UINT_MUL_HIGH:
            return XA_INTRINSIC_BITS_MUL_HIGH;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_LOAD:
            return XA_INTRINSIC_BYTE_SLICE_LOAD;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_STORE:
            return XA_INTRINSIC_BYTE_SLICE_STORE;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_FILL:
            return XA_INTRINSIC_BYTE_SLICE_FILL;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COPY_FROM:
            return XA_INTRINSIC_BYTE_SLICE_COPY;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMPARE:
            return XA_INTRINSIC_BYTE_SLICE_COMPARE;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMMON_PREFIX:
            return XA_INTRINSIC_BYTE_SLICE_COMMON_PREFIX;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_REPEAT_FROM:
            return XA_INTRINSIC_BYTE_SLICE_REPEAT;
        case XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_REINTERPRET:
            return XA_INTRINSIC_BYTE_SLICE_REINTERPRET;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_PTR:
            return XA_INTRINSIC_POD_SLICE_PTR;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_MUT_PTR:
            return XA_INTRINSIC_POD_SLICE_MUT_PTR;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_WINDOW:
            return XA_INTRINSIC_POD_SLICE_WINDOW;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_AS_BYTES:
            return XA_INTRINSIC_POD_SLICE_AS_BYTES;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_FILL:
            return XA_INTRINSIC_POD_SLICE_FILL;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COPY_FROM:
            return XA_INTRINSIC_POD_SLICE_COPY;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COMPARE:
            return XA_INTRINSIC_POD_SLICE_COMPARE;
        case XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_GET:
            return XA_INTRINSIC_POD_SLICE_GET;
        default:
            return XA_INTRINSIC_NONE;
    }
}

static XaIntrinsicId xa_builtin_receiver_intrinsic_id(XrType *receiver, AstNode *callee) {
    if (!receiver || !callee || callee->type != AST_MEMBER_ACCESS || !callee->as.member_access.name)
        return XA_INTRINSIC_NONE;

    const char *name = callee->as.member_access.name;
    XaIntrinsicId compiler_owned = xa_intrinsic_compiler_receiver_method(receiver, name);
    if (compiler_owned != XA_INTRINSIC_NONE)
        return compiler_owned;
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (!xa_builtin_receiver_matches_type(receiver, spec->receiver) ||
            strcmp(spec->source_name, name) != 0)
            continue;
        return xa_builtin_receiver_method_intrinsic_id(spec->method_id);
    }
    return XA_INTRINSIC_NONE;
}

static const XaResolvedCall *xa_record_resolved_intrinsic_call(XaInferContext *ctx, AstNode *node,
                                                               AstNode *callee,
                                                               XrType *receiver_type,
                                                               XaSymbol *fallback_symbol,
                                                               XaSymbolLinks *fallback_links) {
    if (!ctx || !ctx->analyzer || !node || !ctx->analyzer->resolved_call_table)
        return NULL;
    const XaSelection *selection = xa_analyzer_get_selection(ctx->analyzer, callee);
    XaSymbol *target = selection ? selection->target_symbol : fallback_symbol;
    XaSymbolLinks *links = target ? xa_analyzer_get_links(ctx->analyzer, target) : fallback_links;
    if (callee && callee->type == AST_MEMBER_ACCESS && callee->as.member_access.object) {
        XrType *recorded_receiver =
            xa_analyzer_get_node_type(ctx->analyzer, callee->as.member_access.object);
        if (recorded_receiver && !XR_TYPE_IS_UNKNOWN(recorded_receiver) &&
            !XR_TYPE_IS_ERROR(recorded_receiver))
            receiver_type = recorded_receiver;
    }
    XaIntrinsicId intrinsic_id = links ? links->intrinsic_id : XA_INTRINSIC_NONE;
    if (intrinsic_id == XA_INTRINSIC_NONE)
        intrinsic_id = xa_intrinsic_compiler_receiver_method(
            receiver_type,
            callee && callee->type == AST_MEMBER_ACCESS ? callee->as.member_access.name : NULL);
    if (intrinsic_id == XA_INTRINSIC_NONE && callee && callee->type == AST_MEMBER_ACCESS) {
        const char *module_name = xa_call_object_module_name(ctx, callee->as.member_access.object);
        const char *member_name = callee->as.member_access.name;
        if (module_name && member_name && module_name[0] != '.' && module_name[0] != '/') {
            char key[192];
            int key_len = snprintf(key, sizeof(key), "%s.%s", module_name, member_name);
            const XaIntrinsicDesc *desc =
                key_len > 0 && (size_t) key_len < sizeof(key) ? xa_intrinsic_by_key(key) : NULL;
            if (desc)
                intrinsic_id = desc->id;
        }
    }
    if (intrinsic_id == XA_INTRINSIC_NONE && (!target || target->is_builtin))
        intrinsic_id = xa_builtin_receiver_intrinsic_id(receiver_type, callee);
    if (intrinsic_id == XA_INTRINSIC_NONE)
        return NULL;
    XaResolvedCall resolved = {
        .source_node_id = node->node_id,
        .target_symbol_id = target ? target->id : 0,
        .intrinsic_id = intrinsic_id,
        .reason = XA_RESOLVED_CALL_REASON_RESOLVED,
        .flags =
            ctx->unsafe_depth > 0 ? XA_RESOLVED_CALL_FLAG_UNSAFE_SCOPE : XA_RESOLVED_CALL_FLAG_NONE,
    };
    xa_resolved_call_table_set((XaResolvedCallTable *) ctx->analyzer->resolved_call_table, node,
                               &resolved);
    return xa_analyzer_get_resolved_call(ctx->analyzer, node);
}

static bool xa_codegen_opaque_type_supported(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    if (type->kind == XR_KIND_POINTER)
        return true;
    if (type->kind != XR_KIND_INT)
        return false;
    switch ((XrNativeType) type->scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static void xa_check_codegen_intrinsic_call(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                            const XaIntrinsicDesc *desc) {
    if (!ctx || !ctx->analyzer || !node || !call || !desc ||
        desc->id != XA_INTRINSIC_CODEGEN_OPAQUE || call->arg_count != 1 || !call->arguments[0])
        return;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[0]);
    if (xa_codegen_opaque_type_supported(type))
        return;
    XrLocation loc = {.file = ctx->file_path,
                      .line = call->arguments[0]->line,
                      .column = call->arguments[0]->column};
    xa_analyzer_add_diagnostic(
        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
        "codegen.opaque accepts only non-null integer scalars and Ptr<T>/MutPtr<T>; "
        "aggregate, managed, floating-point, boolean, rune, and nullable values are rejected",
        &loc);
}

static bool xa_freestanding_builtin_call_rejected(const char *name) {
    if (!name)
        return false;
    return strcmp(name, "string") == 0 || strcmp(name, "rune") == 0 || strcmp(name, "chr") == 0 ||
           strcmp(name, "typeName") == 0 || strcmp(name, "dump") == 0;
}

static XrType *xa_call_raw_pointer_type_namespace(XaInferContext *ctx, AstNode *object) {
    if (!ctx || !object || object->type != AST_NEW_EXPR)
        return NULL;
    NewExprNode *ne = &object->as.new_expr;
    if (ne->module_name || !ne->class_name || ne->type_arg_count != 1 || !ne->type_args ||
        !ne->type_args[0])
        return NULL;
    bool is_mut = false;
    if (strcmp(ne->class_name, "Ptr") == 0) {
        is_mut = false;
    } else if (strcmp(ne->class_name, "MutPtr") == 0) {
        is_mut = true;
    } else {
        return NULL;
    }
    XrType *pointee = xr_tref_resolve_in_analyzer(ctx->analyzer, ne->type_args[0]);
    if (xa_reject_error_type_success_type(ctx->analyzer, pointee, "generic type argument",
                                          ne->class_name, object ? object->line : 0,
                                          object ? object->column : 0))
        return xr_type_new_error(NULL);
    if (!pointee)
        pointee = xr_type_new_unknown(ctx->analyzer->isolate);
    return xr_type_new_pointer(ctx->analyzer->isolate, pointee, is_mut);
}

static bool xa_call_is_mem_addr(CallExprNode *call, XaSymbolLinks *fn_links) {
    if (fn_links && fn_links->module_name && strcmp(fn_links->module_name, "mem") == 0) {
        if (fn_links->import_member_name && strcmp(fn_links->import_member_name, "addr") == 0)
            return true;
    }
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || strcmp(ma->name, "addr") != 0 || !ma->object ||
        ma->object->type != AST_VARIABLE)
        return false;
    return ma->object->as.variable.name && strcmp(ma->object->as.variable.name, "mem") == 0;
}

static void xa_check_mem_addr_arg(XaInferContext *ctx, AstNode *arg_node, XrType *arg_type) {
    if (!ctx || !ctx->analyzer || !arg_node)
        return;
    if (arg_type && XR_TYPE_IS_POINTER(arg_type))
        return;
    XrLocation loc = {.file = ctx->file_path, .line = arg_node->line, .column = arg_node->column};
    char msg[192];
    snprintf(msg, sizeof(msg), "mem.addr expects Ptr<T> or MutPtr<T>, got '%s'",
             arg_type ? xr_type_to_string(arg_type) : "unknown");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static void xa_check_class_constructor_args(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                            const char *class_name, XaSymbolLinks *class_links,
                                            XrClassInfo *class_info, XrType **type_args,
                                            int type_arg_count) {
    if (!ctx || !ctx->analyzer || !node || !call || !class_info || call->arg_count <= 0)
        return;
    XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
    XaSymbolLinks *ctor_links = ctor ? xa_analyzer_get_links(ctx->analyzer, ctor) : NULL;
    XrType *ctor_type = ctor_links ? ctor_links->type : NULL;
    if (!ctor_type || !XR_TYPE_IS_FUNCTION(ctor_type)) {
        xa_report_arg_accesses_require_known_contract(ctx, node, call);
        return;
    }

    int ctor_pc = ctor_type->function.param_count;
    int check_count = ctor_pc < call->arg_count ? ctor_pc : call->arg_count;
    if (!ctor_type->function.is_variadic && call->arg_count > ctor_pc) {
        for (int i = ctor_pc; i < call->arg_count; i++) {
            XrCallArgAccess access = xa_call_arg_access(call, i);
            if (access != XR_CALL_ARG_PLAIN)
                xa_report_arg_access_without_matching_contract(
                    ctx, node, call->arguments ? call->arguments[i] : NULL, access, i);
        }
    }
    int class_tp_count = class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
    const char **param_names = NULL;
    const char *param_names_buf[8] = {0};
    bool is_parallel_plan_ctor =
        xa_class_info_is_parallel_plan(ctx, class_info) ||
        (class_name && xa_class_name_matches_mono_base(class_name, "Plan") && class_links &&
         class_links->module_name && strcmp(class_links->module_name, "parallel") == 0);
    if (class_tp_count > 0 && type_args && type_arg_count == class_tp_count) {
        param_names = (class_tp_count <= 8)
                          ? param_names_buf
                          : xr_malloc(sizeof(const char *) * (size_t) class_tp_count);
        if (param_names) {
            for (int i = 0; i < class_tp_count; i++)
                param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
        }
    }

    for (int i = 0; i < check_count; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        if (!arg)
            continue;
        XrType *expected = xr_type_function_param_type(ctor_type, i);
        XrType *resolved = expected;
        if (expected && param_names && type_args && type_arg_count == class_tp_count) {
            resolved = xr_type_substitute(ctx->analyzer->isolate, expected, param_names, type_args,
                                          type_arg_count);
        }
        XrType *saved_expected = ctx->expected_type;
        if (resolved && !XR_TYPE_IS_UNKNOWN(resolved))
            ctx->expected_type = resolved;
        const char *parallel_callback_label =
            is_parallel_plan_ctor && i == 1 ? "parallel.Plan init callback" : NULL;
        XrParamMode param_mode = xr_type_function_param_mode(ctor_type, i);
        XrCallArgAccess access = xa_call_arg_access(call, i);
        XrType *arg_type =
            xa_visit_call_arg_for_param_mode(ctx, arg, parallel_callback_label, access, param_mode);
        ctx->expected_type = saved_expected;
        if (!resolved || XR_TYPE_IS_UNKNOWN(resolved) || !arg_type || XR_TYPE_IS_UNKNOWN(arg_type))
            continue;
        xa_check_call_arg_access_authorization(ctx, node, call, arg, i, i, param_mode);
        if (!xa_call_arg_type_assignable(resolved, arg_type, param_mode)) {
            XrLocation loc = {.file = ctx->file_path, .line = arg->line, .column = arg->column};
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Argument %d: type '%s' is not assignable to constructor parameter type '%s' "
                     "for '%s'",
                     i + 1, xr_type_to_string(arg_type), xr_type_to_string(resolved),
                     class_name ? class_name : "class");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    }

    if (param_names && param_names != param_names_buf)
        xr_free((void *) param_names);
}

static XaSemanticTypeId xa_class_constructor_semantic_type(CallExprNode *call,
                                                           const char *class_name,
                                                           XaSymbolLinks *class_links) {
    if (!call)
        return XA_SEMANTIC_TYPE_NONE;
    XaSemanticTypeId id = (XaSemanticTypeId) call->semantic_type_id;
    if (id != XA_SEMANTIC_TYPE_NONE)
        return id;
    const char *module_name = class_links ? class_links->module_name : NULL;
    const char *member_name = class_links && class_links->import_member_name
                                  ? class_links->import_member_name
                                  : class_name;
    if (!module_name || !member_name || module_name[0] == '.' || module_name[0] == '/')
        return XA_SEMANTIC_TYPE_NONE;
    char key[192];
    int key_len = snprintf(key, sizeof(key), "%s.%s", module_name, member_name);
    if (key_len <= 0 || (size_t) key_len >= sizeof(key))
        return XA_SEMANTIC_TYPE_NONE;
    id = xa_semantic_type_by_key(key);
    if (id != XA_SEMANTIC_TYPE_NONE) {
        call->semantic_type_id = (uint32_t) id;
        call->semantic_type_args = call->type_args;
        call->semantic_type_arg_count = call->type_arg_count;
    }
    return id;
}

static XrType *xa_semantic_constructor_instance(XaInferContext *ctx, CallExprNode *call,
                                                XaSemanticTypeId semantic_type_id,
                                                XrClassInfo *class_info) {
    if (!ctx || !ctx->analyzer || !call || semantic_type_id == XA_SEMANTIC_TYPE_NONE)
        return NULL;
    int count = call->semantic_type_arg_count;
    if (count < 0 || (count > 0 && !call->semantic_type_args))
        return NULL;
    XrType **args = count > 0 ? xr_malloc(sizeof(XrType *) * (size_t) count) : NULL;
    if (count > 0 && !args)
        return NULL;
    for (int i = 0; i < count; i++)
        args[i] = xr_tref_resolve_in_analyzer(ctx->analyzer, call->semantic_type_args[i]);
    const char *source_name = xa_semantic_type_source_name(semantic_type_id);
    XrType *instance = count > 0
                           ? xr_type_new_generic_instance(ctx->analyzer->isolate,
                                                          source_name ? source_name : "<semantic>",
                                                          class_info, args, count)
                           : xr_type_new_named_instance(ctx->analyzer->isolate,
                                                        source_name ? source_name : "<semantic>");
    if (instance && class_info)
        instance->instance.class_ref = class_info;
    xr_free(args);
    if (instance)
        instance->semantic_type_id = (uint32_t) semantic_type_id;
    return instance;
}

static XrType *xa_imported_semantic_class_instance_type(XaInferContext *ctx, AstNode *node,
                                                        CallExprNode *call) {
    if (!ctx || !ctx->analyzer || !call || !call->callee || call->callee->type != AST_VARIABLE ||
        !call->callee->as.variable.name)
        return NULL;
    const char *source_spelling = call->callee->as.variable.name;
    XaSymbol *symbol = xa_lookup_visible_symbol(ctx, source_spelling);
    if (!symbol || (!symbol->is_imported && symbol->kind != XA_SYM_IMPORT))
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    XaSemanticTypeId semantic_type_id =
        xa_class_constructor_semantic_type(call, source_spelling, links);
    if (semantic_type_id == XA_SEMANTIC_TYPE_NONE)
        return NULL;

    int expected_type_args = semantic_type_id == XA_SEMANTIC_TYPE_PARALLEL_PLAN ? 1 : 0;
    if (call->semantic_type_arg_count != expected_type_args) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = node ? node->line : 0,
                          .column = node ? node->column : 0};
        char msg[192];
        snprintf(msg, sizeof(msg), "Generic class '%s' expects %d type argument(s), but got %d",
                 xa_semantic_type_source_name(semantic_type_id), expected_type_args,
                 call->semantic_type_arg_count);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    XrType *instance = xa_semantic_constructor_instance(ctx, call, semantic_type_id, NULL);
    if (!instance)
        return NULL;
    if (semantic_type_id == XA_SEMANTIC_TYPE_PARALLEL_PLAN && call->arg_count > 1 &&
        instance->instance.type_arg_count == 1 && instance->instance.type_args &&
        instance->instance.type_args[0]) {
        XrType *lane_params[] = {xr_type_new_int(ctx->analyzer->isolate)};
        XrType *init_type = xr_type_new_function(ctx->analyzer->isolate, lane_params, 1,
                                                 instance->instance.type_args[0], false);
        XrType *saved_expected = ctx->expected_type;
        ctx->expected_type = init_type;
        xa_visit_call_arg_with_parallel_context(ctx, call->arguments[1],
                                                "parallel.Plan init callback");
        ctx->expected_type = saved_expected;
    }
    return instance;
}

/*
 * task-221 gap C: synthesize a syntax-level XrTypeRef from a resolved XrType.
 *
 * Inferred generic-class construction in call form (`Cell(5)`) resolves its type
 * arguments in the analyzer but, unlike the explicit new form (`Cell<int>(5)`),
 * never records them on the AST node. Monomorphization (collect_instantiation_sites)
 * and AOT cgen key off call->type_args, so the inferred instantiation is never
 * specialized: cgen degrades to a map-backed object and calls a constructor that
 * was never generated.
 *
 * The inferred reference becomes part of the AST, so it uses the compiler
 * session's active AST arena through the XrTypeRef constructors. Those
 * constructors copy temporary child arrays; no analyzer-owned heap graph
 * escapes. Returns NULL for shapes we do not synthesize; the caller then skips
 * the writeback and keeps prior behavior.
 *
 * The rule for adding a shape is round-trip fidelity: synthesize only when
 * resolving the result reproduces the type it came from. Containers
 * (Array/Slice/Set/Map/Json) and tuples qualify, and so does a nullable carrier
 * through XR_TREF_OPTIONAL. Deliberately excluded, because their tref form
 * cannot carry everything XrType holds and the clone would be typed against a
 * different type than the call site inferred:
 *   - function types (throw effect, parameter modes, view-return provenance)
 *   - unions (resolution normalizes and sorts members)
 *   - records and field-shaped Json (per-field readonly, sealed-ness)
 *   - const carriers (xr_type_make_const is not a syntax-level wrapper here)
 * Those still reach the generic origin erased, which stays a correct callable
 * body — see xaot_callable_func_has_executable_body_plan.
 */
static XrTypeRef *xa_synth_tref_from_type(XrCompilerSession *session, const XrType *t) {
    if (!session || !t)
        return NULL;
    /* A nullable carrier is the same shape one optional level down. Handled
     * before the kind switch so every synthesizable kind gets it uniformly. */
    if (t->is_nullable && !t->is_const) {
        XrType bare = *t;
        bare.is_nullable = false;
        XrTypeRef *inner = xa_synth_tref_from_type(session, &bare);
        return inner ? xr_tref_optional(session, inner) : NULL;
    }
    switch (t->kind) {
        case XR_KIND_INT:
            return t->scalar_rep == XR_NATIVE_I64 ? xr_tref_int(session)
                                                  : xr_tref_int_width(session, t->scalar_rep);
        case XR_KIND_FLOAT:
            if (!xr_scalar_rep_is_float(t->scalar_rep))
                return NULL;
            return t->scalar_rep == XR_NATIVE_F64 ? xr_tref_float(session)
                                                  : xr_tref_float_width(session, t->scalar_rep);
        case XR_KIND_STRING:
            return xr_tref_string(session);
        case XR_KIND_BOOL:
            return xr_tref_bool(session);
        case XR_KIND_RUNE:
            return xr_tref_char(session);
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_SET: {
            if (t->is_const)
                return NULL;
            XrTypeRef *elem = xa_synth_tref_from_type(session, t->container.element_type);
            if (!elem)
                return NULL;
            const char *head = t->kind == XR_KIND_ARRAY   ? "Array"
                               : t->kind == XR_KIND_SLICE ? TYPE_NAME_SLICE
                               : t->is_weak               ? "WeakSet"
                                                          : "Set";
            return xr_tref_generic(session, head, &elem, 1);
        }
        case XR_KIND_MAP: {
            if (t->is_const)
                return NULL;
            XrTypeRef *kv[2];
            kv[0] = xa_synth_tref_from_type(session, t->map.key_type);
            kv[1] = xa_synth_tref_from_type(session, t->map.value_type);
            if (!kv[0] || !kv[1])
                return NULL;
            return xr_tref_generic(session, t->is_weak ? "WeakMap" : "Map", kv, 2);
        }
        case XR_KIND_TUPLE: {
            if (t->is_const)
                return NULL;
            int n = t->tuple.element_count;
            if (n < 0 || n > 255 || (n > 0 && !t->tuple.element_types))
                return NULL;
            XrTypeRef *stack_elems[8] = {0};
            XrTypeRef **elems =
                n <= 8 ? stack_elems : (XrTypeRef **) xr_malloc(sizeof(XrTypeRef *) * (size_t) n);
            if (!elems)
                return NULL;
            bool complete = true;
            for (int i = 0; i < n; i++) {
                elems[i] = xa_synth_tref_from_type(session, t->tuple.element_types[i]);
                if (!elems[i]) {
                    complete = false;
                    break;
                }
            }
            XrTypeRef *result = complete ? xr_tref_tuple(session, elems, n) : NULL;
            if (elems != stack_elems)
                xr_free(elems);
            return result;
        }
        case XR_KIND_JSON:
            /* Only plain `Json`: a field-shaped Json has no NAMED spelling that
             * resolves back to the same shape. */
            if (t->is_const || t->object.field_count != 0)
                return NULL;
            return xr_tref_named(session, "Json");
        case XR_KIND_TYPE_PARAM:
            return t->type_param.name ? xr_tref_type_param(session, t->type_param.name) : NULL;
        case XR_KIND_ENUM:
            return t->enum_type.enum_name ? xr_tref_named(session, t->enum_type.enum_name) : NULL;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE: {
            if (!t->instance.class_name)
                return NULL;
            int n = t->instance.type_arg_count;
            if (n <= 0 || !t->instance.type_args)
                return xr_tref_named(session, t->instance.class_name);
            if (n > 255)
                return NULL;
            XrTypeRef *stack_args[8] = {0};
            XrTypeRef **args =
                n <= 8 ? stack_args : (XrTypeRef **) xr_malloc(sizeof(XrTypeRef *) * (size_t) n);
            if (!args)
                return NULL;
            bool complete = true;
            for (int i = 0; i < n; i++) {
                args[i] = xa_synth_tref_from_type(session, t->instance.type_args[i]);
                if (!args[i]) {
                    complete = false;
                    break;
                }
            }
            XrTypeRef *result =
                complete ? xr_tref_generic(session, t->instance.class_name, args, n) : NULL;
            if (args != stack_args)
                xr_free(args);
            return result;
        }
        default:
            return NULL;
    }
}

/*
 * task-221 gap C: record inferred generic type arguments on the call node so
 * monomorphization and AOT cgen specialize the call identically to the explicit
 * form. Used for both inferred generic-class construction (`Cell(5)`) and
 * inferred generic-function calls (`wrapIt(99)`). Only applies when no explicit
 * type args were written and every inferred arg synthesizes; otherwise leaves
 * the node untouched.
 */
void xa_writeback_inferred_type_args(XrCompilerSession *session, CallExprNode *call,
                                     XrType **inferred, int type_param_count) {
    if (!session || !call || !inferred || type_param_count <= 0 || call->type_arg_count != 0)
        return;
    XrTypeRef *stack_synth[8] = {0};
    XrTypeRef **synth =
        type_param_count <= 8
            ? stack_synth
            : (XrTypeRef **) xr_malloc(sizeof(XrTypeRef *) * (size_t) type_param_count);
    if (!synth)
        return;
    for (int i = 0; i < type_param_count; i++) {
        synth[i] = xa_synth_tref_from_type(session, inferred[i]);
        if (!synth[i]) {
            if (synth != stack_synth)
                xr_free(synth);
            return;
        }
    }
    call->type_args = xr_tref_array_copy(session, synth, type_param_count);
    if (synth != stack_synth)
        xr_free(synth);
    call->type_arg_count = type_param_count;
    call->semantic_type_args = call->type_args;
    call->semantic_type_arg_count = type_param_count;
}

static XrType *xa_class_constructor_instance_type(XaInferContext *ctx, AstNode *node,
                                                  CallExprNode *call, const char *class_name,
                                                  XaSymbolLinks *class_links,
                                                  XrClassInfo *class_info) {
    if (!ctx || !ctx->analyzer || !class_info)
        return xr_type_new_error(NULL);

    xa_check_constructor_visibility(ctx, node, class_info);
    XaSemanticTypeId semantic_type_id =
        xa_class_constructor_semantic_type(call, class_name, class_links);

    bool is_value_type = class_links && class_links->type && class_links->type->is_value_type;
    int type_param_count = class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
    if (call && type_param_count > 0 && call->type_arg_count == 0 &&
        semantic_type_id != XA_SEMANTIC_TYPE_NONE &&
        call->semantic_type_arg_count == type_param_count && call->semantic_type_args) {
        XrType *resolved_buf[8] = {0};
        XrType **resolved = type_param_count <= 8
                                ? resolved_buf
                                : xr_malloc(sizeof(XrType *) * (size_t) type_param_count);
        if (!resolved)
            return xr_type_new_error(ctx->analyzer->isolate);
        for (int i = 0; i < type_param_count; i++)
            resolved[i] = xr_tref_resolve_in_analyzer(ctx->analyzer, call->semantic_type_args[i]);
        xa_check_span_generic_class_type_args(ctx, node, class_name, resolved, type_param_count);
        xa_check_class_constructor_args(ctx, node, call, class_name, class_links, class_info,
                                        resolved, type_param_count);
        const char *source_name = xa_semantic_type_source_name(semantic_type_id);
        XrType *inst = xr_type_new_generic_instance(ctx->analyzer->isolate,
                                                    source_name ? source_name : class_name,
                                                    class_info, resolved, type_param_count);
        if (resolved != resolved_buf)
            xr_free(resolved);
        if (inst) {
            inst->semantic_type_id = (uint32_t) semantic_type_id;
            if (is_value_type)
                inst->is_value_type = true;
        }
        return inst ? inst : xr_type_new_error(ctx->analyzer->isolate);
    }
    if (call && type_param_count > 0) {
        if (call->type_arg_count > 0) {
            if (call->type_arg_count != type_param_count) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Generic class '%s' expects %d type argument(s), but got %d",
                         class_name ? class_name : "class", type_param_count, call->type_arg_count);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
                return xr_type_new_error(ctx->analyzer->isolate);
            } else {
                XrType *resolved_buf[8] = {0};
                XrType **resolved =
                    (call->type_arg_count <= 8)
                        ? resolved_buf
                        : xr_malloc(sizeof(XrType *) * (size_t) call->type_arg_count);
                if (resolved) {
                    for (int i = 0; i < call->type_arg_count; i++) {
                        resolved[i] =
                            call->type_args[i]
                                ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[i])
                                : xr_type_new_error(NULL);
                    }
                    bool poisoned_type_arg = false;
                    for (int i = 0; i < call->type_arg_count; i++) {
                        if (xa_reject_error_type_success_type(
                                ctx->analyzer, resolved[i], "generic type argument",
                                class_name ? class_name : "class", node ? node->line : 0,
                                node ? node->column : 0)) {
                            poisoned_type_arg = true;
                        }
                    }
                    if (poisoned_type_arg) {
                        if (resolved != resolved_buf)
                            xr_free(resolved);
                        return xr_type_new_error(NULL);
                    }
                    xa_check_span_generic_class_type_args(ctx, node, class_name, resolved,
                                                          call->type_arg_count);
                    xa_check_class_constructor_args(ctx, node, call, class_name, class_links,
                                                    class_info, resolved, call->type_arg_count);
                    /* The mono pass rewrites `Box<int>(...)` to `Box$i64(...)` but keeps the
                     * source type arguments for diagnostics.  The rewritten declaration is no
                     * longer a generic class, so representing the result as another generic
                     * instance creates two identities for the same value (`Box<int>` and
                     * `Box$i64`) and breaks invariant containers such as `Array<Box<int>>`.
                     * Preserve validation against the original type arguments above, then use
                     * the concrete clone's nominal instance as the expression type. */
                    XrType *inst = class_name && strchr(class_name, '$')
                                       ? xr_type_new_instance(ctx->analyzer->isolate, class_info)
                                       : xr_type_new_generic_instance(
                                             ctx->analyzer->isolate, class_name, class_info,
                                             resolved, call->type_arg_count);
                    if (inst && is_value_type)
                        inst->is_value_type = true;
                    if (inst && semantic_type_id != XA_SEMANTIC_TYPE_NONE)
                        inst->semantic_type_id = (uint32_t) semantic_type_id;
                    if (resolved != resolved_buf)
                        xr_free(resolved);
                    if (inst)
                        return inst;
                }
            }
        } else if (call->arg_count > 0) {
            XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
            XaSymbolLinks *ctor_links = ctor ? xa_analyzer_get_links(ctx->analyzer, ctor) : NULL;
            XrType *ctor_type = ctor_links ? ctor_links->type : NULL;
            if (ctor_type && XR_TYPE_IS_FUNCTION(ctor_type)) {
                XrType *inferred_buf[8] = {0};
                XrType **inferred = (type_param_count <= 8)
                                        ? inferred_buf
                                        : xr_calloc((size_t) type_param_count, sizeof(XrType *));
                if (inferred) {
                    bool all_inferred = true;
                    for (int ti = 0; ti < type_param_count; ti++) {
                        const char *tp_name = xa_symbol_links_get_type_param_name(class_links, ti);
                        if (!tp_name) {
                            all_inferred = false;
                            break;
                        }
                        for (int pi = 0;
                             pi < ctor_type->function.param_count && pi < call->arg_count; pi++) {
                            XrType *pt = xr_type_function_param_type(ctor_type, pi);
                            XrCallArgAccess access = xa_call_arg_access(call, pi);
                            XrParamMode param_mode = xr_type_function_param_mode(ctor_type, pi);
                            XrType *arg_type =
                                call->arguments[pi]
                                    ? xa_visit_call_arg_for_param_mode(ctx, call->arguments[pi],
                                                                       NULL, access, param_mode)
                                    : NULL;
                            inferred[ti] = xa_infer_type_param_from_arg(pt, arg_type, tp_name, 0);
                            if (inferred[ti])
                                break;
                        }
                        if (!inferred[ti]) {
                            all_inferred = false;
                            break;
                        }
                    }
                    if (all_inferred) {
                        bool poisoned_type_arg = false;
                        for (int i = 0; i < type_param_count; i++) {
                            if (xa_reject_error_type_success_type(
                                    ctx->analyzer, inferred[i], "generic type argument",
                                    class_name ? class_name : "class", node ? node->line : 0,
                                    node ? node->column : 0)) {
                                poisoned_type_arg = true;
                            }
                        }
                        if (poisoned_type_arg) {
                            if (inferred != inferred_buf)
                                xr_free(inferred);
                            return xr_type_new_error(NULL);
                        }
                        xa_check_span_generic_class_type_args(ctx, node, class_name, inferred,
                                                              type_param_count);
                        xa_check_class_constructor_args(ctx, node, call, class_name, class_links,
                                                        class_info, inferred, type_param_count);
                        XrType *inst =
                            xr_type_new_generic_instance(ctx->analyzer->isolate, class_name,
                                                         class_info, inferred, type_param_count);
                        if (inst && is_value_type)
                            inst->is_value_type = true;
                        if (inst && semantic_type_id != XA_SEMANTIC_TYPE_NONE)
                            inst->semantic_type_id = (uint32_t) semantic_type_id;
                        if (inst)
                            xa_writeback_inferred_type_args(ctx->analyzer->compiler_session, call,
                                                            inferred, type_param_count);
                        if (inferred != inferred_buf)
                            xr_free(inferred);
                        if (inst)
                            return inst;
                    }
                    if (inferred != inferred_buf)
                        xr_free(inferred);
                }
            }
        }

        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        XaInferVar *var = xa_infer_var_new(ctx, "generic constructor type arguments", &loc);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "cannot infer type arguments for generic constructor '%s'; add explicit %s<T> "
                 "type arguments",
                 class_name ? class_name : "class", class_name ? class_name : "constructor");
        return xa_infer_var_report_unsolved(ctx, var, msg);
    }

    xa_check_class_constructor_args(ctx, node, call, class_name, class_links, class_info, NULL, 0);
    XrType *semantic_instance =
        xa_semantic_constructor_instance(ctx, call, semantic_type_id, class_info);
    if (semantic_instance) {
        if (is_value_type)
            semantic_instance->is_value_type = true;
        return semantic_instance;
    }
    XrType *inst = xr_type_new_instance(ctx->analyzer->isolate, class_info);
    if (inst && is_value_type)
        inst->is_value_type = true;
    if (inst && semantic_type_id != XA_SEMANTIC_TYPE_NONE)
        inst->semantic_type_id = (uint32_t) semantic_type_id;
    return inst;
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
        XrType *want = xr_type_function_param_type(callback_type, i);
        XrType *got = xr_type_function_param_type(arg_type, i);
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

static const XaEnumVariantInfo *xa_call_payload_enum_variant(XaInferContext *ctx,
                                                             const CallExprNode *call,
                                                             const char **enum_name_out,
                                                             const char **variant_name_out) {
    if (enum_name_out)
        *enum_name_out = NULL;
    if (variant_name_out)
        *variant_name_out = NULL;
    if (!ctx || !ctx->analyzer || !call || !call->callee)
        return NULL;

    const char *enum_name = NULL;
    const char *variant_name = NULL;
    if (call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        if (!ma->object || ma->object->type != AST_VARIABLE || !ma->name)
            return NULL;
        enum_name = ma->object->as.variable.name;
        variant_name = ma->name;
    } else if (call->callee->type == AST_ENUM_ACCESS) {
        EnumAccessNode *ea = &call->callee->as.enum_access;
        enum_name = ea->enum_name;
        variant_name = ea->member_name;
    } else {
        return NULL;
    }
    if (!enum_name || !variant_name)
        return NULL;

    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, enum_name);
    if (!sym || sym->kind != XA_SYM_ENUM)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    XaEnumInfo *info = links ? links->enum_info : NULL;
    int idx = xa_enum_info_find_variant(info, variant_name);
    if (idx < 0 || !info->variants || info->variants[idx].payload_count == 0)
        return NULL;

    if (enum_name_out)
        *enum_name_out = enum_name;
    if (variant_name_out)
        *variant_name_out = variant_name;
    return &info->variants[idx];
}

static void xa_check_payload_enum_variant_call(XaInferContext *ctx, AstNode *node,
                                               const CallExprNode *call,
                                               const XaEnumVariantInfo *variant,
                                               const char *enum_name, const char *variant_name) {
    if (!ctx || !node || !call || !variant)
        return;

    int expected = (int) variant->payload_count;
    if (call->arg_count != expected) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "payload enum variant '%s.%s' expects %d argument(s), but got %d",
                 enum_name ? enum_name : "?", variant_name ? variant_name : "?", expected,
                 call->arg_count);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
    }

    int n = call->arg_count < expected ? call->arg_count : expected;
    for (int i = 0; i < n; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        if (!arg)
            continue;
        if (arg->type == AST_SPREAD_EXPR) {
            XrLocation loc = {.file = ctx->file_path, .line = arg->line, .column = arg->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                "payload enum variant constructors require explicit payload arguments", &loc);
            continue;
        }

        XrType *param_type = variant->payload_types ? variant->payload_types[i] : NULL;
        XrType *saved_expected = ctx->expected_type;
        XrType *saved_from_signature = ctx->expected_from_signature;
        if (param_type && !XR_TYPE_IS_UNKNOWN(param_type))
            ctx->expected_type = param_type;
        ctx->expected_from_signature = ctx->expected_type;
        XrType *arg_type = xa_visit_infer_expr(ctx, arg);
        ctx->expected_type = saved_expected;
        ctx->expected_from_signature = saved_from_signature;
        if (xa_type_contains_span_view(arg_type)) {
            char context[160];
            snprintf(context, sizeof(context), "store Slice view in enum payload '%s.%s'",
                     enum_name ? enum_name : "?", variant_name ? variant_name : "?");
            xa_check_span_value_escape(ctx, arg, arg_type, context);
        }
        if (arg_type && XR_TYPE_IS_POINTER(arg_type)) {
            char context[160];
            snprintf(context, sizeof(context), "store raw pointer borrow in enum payload '%s.%s'",
                     enum_name ? enum_name : "?", variant_name ? variant_name : "?");
            xa_check_pointer_borrow_escape(ctx, arg, arg, arg_type, context);
        }
        xa_note_owner_escapes_into_heap(ctx, arg);
        if (!param_type || XR_TYPE_IS_UNKNOWN(param_type) || !arg_type ||
            XR_TYPE_IS_UNKNOWN(arg_type))
            continue;

        XrLocation loc = {.file = ctx->file_path, .line = arg->line, .column = arg->column};
        bool null_err = xa_check_null_safety(ctx->analyzer, param_type, arg_type, "Argument", &loc);
        if (!null_err && !xa_typecheck_assignable(param_type, arg_type) &&
            !xr_is_json_coercion(param_type, arg_type)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Argument %d: type '%s' is not assignable to enum payload type '%s'", i + 1,
                     xr_type_to_string(arg_type), xr_type_to_string(param_type));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
    }
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
    XaSymbol *call_stack[32];
    struct {
        const char *name;
        const char *class_name;
        int block_depth;
    } local_sync_vars[64];
    char feature_buf[128];
    int local_sync_var_count;
    int call_depth;
    int block_depth;
    int nested_function_depth;
    bool report;
    bool found;
    bool reported;
} XaThreadSpawnSyncScan;

static AstNode *xa_thread_spawn_inline_body(AstNode *body);
static XaSymbol *xa_lookup_visible_class_symbol(XaInferContext *ctx, const char *class_name);

typedef enum XaThreadSpawnFunctionValueStatus {
    XA_THREAD_SPAWN_FN_VALUE_SAFE,
    XA_THREAD_SPAWN_FN_VALUE_MAY_SUSPEND,
    XA_THREAD_SPAWN_FN_VALUE_DYNAMIC,
} XaThreadSpawnFunctionValueStatus;

static XaScope *xa_find_function_scope_for_symbol(XaScope *scope, XaSymbol *sym) {
    if (!scope || !sym)
        return NULL;
    if (scope->function_symbol == sym)
        return scope;
    for (int i = 0; i < scope->child_count; i++) {
        XaScope *found = xa_find_function_scope_for_symbol(scope->children[i], sym);
        if (found)
            return found;
    }
    return NULL;
}

static AstNode *xa_symbol_function_body(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !ctx->analyzer || !sym || sym->kind != XA_SYM_FUNCTION)
        return NULL;
    XaScope *scope = xa_find_function_scope_for_symbol(ctx->analyzer->global_scope, sym);
    AstNode *fn_node = scope ? (AstNode *) scope->ast_node : NULL;
    if (!fn_node)
        return NULL;
    if (fn_node->type == AST_FUNCTION_DECL)
        return fn_node->as.function_decl.body;
    if (fn_node->type == AST_FUNCTION_EXPR)
        return fn_node->as.function_expr.body;
    return NULL;
}

XaSymbol *xa_resolve_variable_symbol(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || node->type != AST_VARIABLE)
        return NULL;
    /* The id recorded when the reference was resolved is the only scope-
     * independent answer.  Effect scans walk a body long after its scope was
     * popped, so a name lookup from the current scope would silently skip the
     * shadowing declaration and resolve to an outer symbol of the same name. */
    uint32_t symbol_id = node->as.variable.symbol_id;
    if (symbol_id != 0) {
        XaSymbol *sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, symbol_id);
        if (sym)
            return sym;
    }
    const char *name = node->as.variable.name;
    return name ? xa_lookup_visible_symbol(ctx, name) : NULL;
}

static XaSymbol *xa_thread_spawn_import_target_symbol(XaInferContext *ctx, XaSymbol *sym) {
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

static XaSymbol *xa_thread_spawn_module_member_symbol(XaInferContext *ctx, AstNode *callee) {
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
    bool is_quoted = mod_name[0] == '.' || mod_name[0] == '/';
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, mod_name, is_quoted);
    if (!exports)
        return NULL;

    XaSymbol *member_sym = (XaSymbol *) xr_hashmap_get(exports, ma->name);
    return member_sym;
}

static bool xa_thread_spawn_path_is_sync_module(const char *path) {
    return path && (strstr(path, "stdlib/sync/sync.xr") || strstr(path, "stdlib\\sync\\sync.xr") ||
                    strstr(path, "<embedded stdlib>/sync/sync.xr"));
}
static bool xa_thread_spawn_sync_class_name(const char *class_name) {
    return class_name && (strcmp(class_name, "Mutex") == 0 || strcmp(class_name, "RwLock") == 0 ||
                          strcmp(class_name, "Once") == 0 || strcmp(class_name, "Barrier") == 0 ||
                          strcmp(class_name, "Condvar") == 0);
}

static bool xa_thread_spawn_sync_method_name(const char *class_name, const char *method_name) {
    if (!xa_thread_spawn_sync_class_name(class_name) || !method_name)
        return false;
    if (strcmp(class_name, "Mutex") == 0)
        return strcmp(method_name, "lock") == 0 || strcmp(method_name, "tryLock") == 0 ||
               strcmp(method_name, "replace") == 0;
    if (strcmp(class_name, "RwLock") == 0)
        return strcmp(method_name, "read") == 0 || strcmp(method_name, "write") == 0 ||
               strcmp(method_name, "replace") == 0;
    if (strcmp(class_name, "Once") == 0)
        return strcmp(method_name, "call") == 0;
    if (strcmp(class_name, "Barrier") == 0)
        return strcmp(method_name, "wait") == 0;
    if (strcmp(class_name, "Condvar") == 0)
        return strcmp(method_name, "lock") == 0 || strcmp(method_name, "unlock") == 0 ||
               strcmp(method_name, "wait") == 0 || strcmp(method_name, "signal") == 0 ||
               strcmp(method_name, "broadcast") == 0;
    return false;
}

static bool xa_thread_spawn_module_alias_is_sync(XaInferContext *ctx, const char *name) {
    if (!ctx || !ctx->analyzer || !name)
        return false;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
    if (!sym || sym->kind != XA_SYM_MODULE)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    const char *module_name = links && links->module_name ? links->module_name : name;
    return module_name && strcmp(module_name, "sync") == 0;
}

static const char *xa_thread_spawn_type_sync_class_name(XaInferContext *ctx, XrType *type) {
    if (!ctx || !type || !XR_TYPE_IS_INSTANCE(type) || !type->instance.class_name ||
        !xa_thread_spawn_sync_class_name(type->instance.class_name))
        return NULL;

    XrClassInfo *class_info = type->instance.class_ref;
    if (class_info) {
        for (int i = 0; i < class_info->method_count; i++) {
            XaSymbol *m = class_info->methods ? class_info->methods[i] : NULL;
            XaSymbolLinks *links = m ? xa_analyzer_get_links(ctx->analyzer, m) : NULL;
            if (links && xa_thread_spawn_path_is_sync_module(links->file_path))
                return type->instance.class_name;
        }
    }

    XaSymbol *class_sym = xa_lookup_visible_class_symbol(ctx, type->instance.class_name);
    XaSymbolLinks *class_links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
    return class_links && xa_thread_spawn_path_is_sync_module(class_links->file_path)
               ? type->instance.class_name
               : NULL;
}

static const char *xa_thread_spawn_expr_sync_ctor_class(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !expr)
        return NULL;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    const char *typed_class = xa_thread_spawn_type_sync_class_name(ctx, type);
    if (typed_class)
        return typed_class;

    if (expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        AstNode *callee = call->callee;
        if (!callee)
            return NULL;
        if (callee->type == AST_VARIABLE) {
            const char *name = callee->as.variable.name;
            XaSymbol *sym = xa_lookup_visible_symbol(ctx, name);
            XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
            if (xa_thread_spawn_sync_class_name(name) && sym && sym->is_imported)
                return name;
            XaSymbol *class_sym =
                sym && sym->kind == XA_SYM_CLASS ? sym : xa_lookup_visible_class_symbol(ctx, name);
            links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
            if (xa_thread_spawn_sync_class_name(name) && links &&
                xa_thread_spawn_path_is_sync_module(links->file_path))
                return name;
        } else if (callee->type == AST_MEMBER_ACCESS) {
            MemberAccessNode *ma = &callee->as.member_access;
            if (ma->object && ma->object->type == AST_VARIABLE &&
                xa_thread_spawn_module_alias_is_sync(ctx, ma->object->as.variable.name) &&
                xa_thread_spawn_sync_class_name(ma->name))
                return ma->name;
        }
    } else if (expr->type == AST_NEW_EXPR) {
        NewExprNode *ne = &expr->as.new_expr;
        if (!xa_thread_spawn_sync_class_name(ne->class_name))
            return NULL;
        if (ne->module_name && xa_thread_spawn_module_alias_is_sync(ctx, ne->module_name))
            return ne->class_name;
        XaSymbol *class_sym = xa_lookup_visible_class_symbol(ctx, ne->class_name);
        XaSymbolLinks *links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
        if (!ne->module_name && links && xa_thread_spawn_path_is_sync_module(links->file_path))
            return ne->class_name;
    }
    return NULL;
}

static void xa_thread_spawn_scan_note_local(XaThreadSpawnSyncScan *scan, const char *name,
                                            const char *class_name) {
    if (!scan || !name || scan->local_sync_var_count >= 64)
        return;
    scan->local_sync_vars[scan->local_sync_var_count].name = name;
    scan->local_sync_vars[scan->local_sync_var_count].class_name = class_name;
    scan->local_sync_vars[scan->local_sync_var_count].block_depth = scan->block_depth;
    scan->local_sync_var_count++;
}

static const char *xa_thread_spawn_scan_lookup_local(XaThreadSpawnSyncScan *scan,
                                                     const char *name) {
    if (!scan || !name)
        return NULL;
    for (int i = scan->local_sync_var_count - 1; i >= 0; i--) {
        if (scan->local_sync_vars[i].name && strcmp(scan->local_sync_vars[i].name, name) == 0)
            return scan->local_sync_vars[i].class_name;
    }
    return NULL;
}

static void xa_thread_spawn_scan_pop_block(XaThreadSpawnSyncScan *scan) {
    if (!scan)
        return;
    while (scan->local_sync_var_count > 0 &&
           scan->local_sync_vars[scan->local_sync_var_count - 1].block_depth == scan->block_depth) {
        scan->local_sync_var_count--;
    }
}

static bool xa_thread_spawn_call_is_sync_method(XaThreadSpawnSyncScan *scan, CallExprNode *call) {
    if (!scan || !scan->ctx || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || !ma->object)
        return false;

    XrType *receiver_type = xa_analyzer_get_node_type(scan->ctx->analyzer, ma->object);
    const char *class_name = xa_thread_spawn_type_sync_class_name(scan->ctx, receiver_type);
    if (!class_name && ma->object->type == AST_VARIABLE)
        class_name = xa_thread_spawn_scan_lookup_local(scan, ma->object->as.variable.name);
    if (!class_name)
        class_name = xa_thread_spawn_expr_sync_ctor_class(scan->ctx, ma->object);
    return xa_thread_spawn_sync_method_name(class_name, ma->name);
}

static void xa_report_thread_spawn_suspend(XaThreadSpawnSyncScan *scan, AstNode *site,
                                           const char *feature) {
    if (!scan || scan->reported || !scan->ctx || !site || !feature)
        return;
    XrLocation loc = {.file = scan->ctx->file_path, .line = site->line, .column = site->column};
    char msg[192];
    snprintf(msg, sizeof(msg),
             "sys.Thread.spawn body cannot suspend; %s is not allowed in an OS thread body",
             feature);
    xa_analyzer_add_diagnostic(scan->ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE,
                               msg, &loc);
    scan->reported = true;
}

static bool xa_thread_spawn_body_may_suspend(XaInferContext *ctx, AstNode *body,
                                             XaSymbol **call_stack, int call_depth);

static bool xa_thread_spawn_symbol_may_suspend(XaInferContext *ctx, XaSymbol *sym,
                                               XaSymbol **call_stack, int call_depth) {
    sym = xa_thread_spawn_import_target_symbol(ctx, sym);
    if (!ctx || !sym || sym->kind != XA_SYM_FUNCTION || sym->is_builtin)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links && links->is_extern)
        return false;
    for (int i = 0; i < call_depth; i++) {
        if (call_stack[i] == sym)
            return false;
    }
    if (call_depth >= 32)
        return false;
    AstNode *body = xa_symbol_function_body(ctx, sym);
    if (!body)
        return false;
    call_stack[call_depth] = sym;
    bool result = xa_thread_spawn_body_may_suspend(ctx, body, call_stack, call_depth + 1);
    call_stack[call_depth] = NULL;
    return result;
}

static XaThreadSpawnFunctionValueStatus
xa_thread_spawn_function_value_expr_status(XaInferContext *ctx, AstNode *expr,
                                           XaSymbol **call_stack, int call_depth);

static bool xa_thread_spawn_call_stack_contains(XaSymbol **call_stack, int call_depth,
                                                XaSymbol *sym) {
    if (!call_stack || !sym)
        return false;
    for (int i = 0; i < call_depth; i++) {
        if (call_stack[i] == sym)
            return true;
    }
    return false;
}

static bool xa_thread_spawn_symbol_has_function_type(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !ctx->analyzer || !sym)
        return false;
    XrType *type = xa_analyzer_get_type(ctx->analyzer, sym);
    return type && XR_TYPE_IS_FUNCTION(type);
}

static XaThreadSpawnFunctionValueStatus
xa_thread_spawn_function_value_symbol_status(XaInferContext *ctx, XaSymbol *sym,
                                             XaSymbol **call_stack, int call_depth) {
    sym = xa_thread_spawn_import_target_symbol(ctx, sym);
    if (!ctx || !sym)
        return XA_THREAD_SPAWN_FN_VALUE_SAFE;

    if (sym->kind == XA_SYM_FUNCTION) {
        return xa_thread_spawn_symbol_may_suspend(ctx, sym, call_stack, call_depth)
                   ? XA_THREAD_SPAWN_FN_VALUE_MAY_SUSPEND
                   : XA_THREAD_SPAWN_FN_VALUE_SAFE;
    }

    if (sym->kind != XA_SYM_VARIABLE && sym->kind != XA_SYM_PARAMETER)
        return XA_THREAD_SPAWN_FN_VALUE_SAFE;
    if (!xa_thread_spawn_symbol_has_function_type(ctx, sym))
        return XA_THREAD_SPAWN_FN_VALUE_SAFE;

    if (sym->kind == XA_SYM_PARAMETER || !sym->is_const)
        return XA_THREAD_SPAWN_FN_VALUE_DYNAMIC;
    if (xa_thread_spawn_call_stack_contains(call_stack, call_depth, sym))
        return XA_THREAD_SPAWN_FN_VALUE_DYNAMIC;
    if (call_depth >= 32)
        return XA_THREAD_SPAWN_FN_VALUE_DYNAMIC;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || !links->const_initializer)
        return XA_THREAD_SPAWN_FN_VALUE_DYNAMIC;

    call_stack[call_depth] = sym;
    XaThreadSpawnFunctionValueStatus status = xa_thread_spawn_function_value_expr_status(
        ctx, links->const_initializer, call_stack, call_depth + 1);
    call_stack[call_depth] = NULL;
    return status;
}

static XaThreadSpawnFunctionValueStatus
xa_thread_spawn_function_value_expr_status(XaInferContext *ctx, AstNode *expr,
                                           XaSymbol **call_stack, int call_depth) {
    if (!ctx || !expr)
        return XA_THREAD_SPAWN_FN_VALUE_DYNAMIC;

    AstNode *inline_body = xa_thread_spawn_inline_body(expr);
    if (inline_body) {
        return xa_thread_spawn_body_may_suspend(ctx, inline_body, call_stack, call_depth)
                   ? XA_THREAD_SPAWN_FN_VALUE_MAY_SUSPEND
                   : XA_THREAD_SPAWN_FN_VALUE_SAFE;
    }

    if (expr->type == AST_VARIABLE) {
        XaSymbol *sym = xa_resolve_variable_symbol(ctx, expr);
        return xa_thread_spawn_function_value_symbol_status(ctx, sym, call_stack, call_depth);
    }

    if (expr->type == AST_MEMBER_ACCESS) {
        XaSymbol *sym = xa_thread_spawn_module_member_symbol(ctx, expr);
        if (sym)
            return xa_thread_spawn_function_value_symbol_status(ctx, sym, call_stack, call_depth);
        XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
        if (!type)
            type = xa_visit_infer_expr(ctx, expr);
        return type && XR_TYPE_IS_FUNCTION(type) ? XA_THREAD_SPAWN_FN_VALUE_DYNAMIC
                                                 : XA_THREAD_SPAWN_FN_VALUE_SAFE;
    }

    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    if (!type)
        type = xa_visit_infer_expr(ctx, expr);
    return type && XR_TYPE_IS_FUNCTION(type) ? XA_THREAD_SPAWN_FN_VALUE_DYNAMIC
                                             : XA_THREAD_SPAWN_FN_VALUE_SAFE;
}

static bool xa_thread_spawn_inline_call_may_suspend(XaInferContext *ctx, CallExprNode *call,
                                                    XaSymbol **call_stack, int call_depth,
                                                    const char **out_feature) {
    if (out_feature)
        *out_feature = NULL;
    if (!ctx || !call)
        return false;
    if (xa_call_is_builtin_module_member(ctx, call, "Coro", "yield")) {
        if (out_feature)
            *out_feature = "Coro.yield()";
        return true;
    }

    AstNode *callee = call->callee;
    AstNode *inline_body = xa_thread_spawn_inline_body(callee);
    if (inline_body && xa_thread_spawn_body_may_suspend(ctx, inline_body, call_stack, call_depth)) {
        if (out_feature)
            *out_feature = "call to suspendable inline function";
        return true;
    }

    if (callee && callee->type == AST_VARIABLE) {
        XaSymbol *sym = xa_resolve_variable_symbol(ctx, callee);
        if (sym && sym->kind == XA_SYM_FUNCTION) {
            if (xa_thread_spawn_symbol_may_suspend(ctx, sym, call_stack, call_depth)) {
                if (out_feature)
                    *out_feature = NULL;
                return true;
            }
        } else {
            XaThreadSpawnFunctionValueStatus status =
                xa_thread_spawn_function_value_symbol_status(ctx, sym, call_stack, call_depth);
            if (status == XA_THREAD_SPAWN_FN_VALUE_MAY_SUSPEND) {
                if (out_feature)
                    *out_feature = NULL;
                return true;
            }
            if (status == XA_THREAD_SPAWN_FN_VALUE_DYNAMIC) {
                if (out_feature)
                    *out_feature = "call through dynamic function value";
                return true;
            }
        }
    } else if (callee && callee->type == AST_MEMBER_ACCESS) {
        XaSymbol *sym = xa_thread_spawn_module_member_symbol(ctx, callee);
        if (!sym) {
            return false;
        } else if (sym->kind == XA_SYM_FUNCTION) {
            if (xa_thread_spawn_symbol_may_suspend(ctx, sym, call_stack, call_depth)) {
                if (out_feature)
                    *out_feature = NULL;
                return true;
            }
        } else {
            XaThreadSpawnFunctionValueStatus status =
                xa_thread_spawn_function_value_symbol_status(ctx, sym, call_stack, call_depth);
            if (status == XA_THREAD_SPAWN_FN_VALUE_MAY_SUSPEND) {
                if (out_feature)
                    *out_feature = NULL;
                return true;
            }
            if (status == XA_THREAD_SPAWN_FN_VALUE_DYNAMIC) {
                if (out_feature)
                    *out_feature = "call through dynamic function value";
                return true;
            }
        }
    }
    return false;
}

static void xa_thread_spawn_sync_scan_pre(AstNode *node, void *ud) {
    XaThreadSpawnSyncScan *scan = (XaThreadSpawnSyncScan *) ud;
    if (!scan || scan->found || scan->reported || !scan->ctx || !node)
        return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL) {
        scan->nested_function_depth++;
        return;
    }
    if (scan->nested_function_depth > 0)
        return;
    if (node->type == AST_BLOCK)
        scan->block_depth++;

    if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) {
        VarDeclNode *var = &node->as.var_decl;
        const char *class_name =
            xa_thread_spawn_expr_sync_ctor_class(scan->ctx, var ? var->initializer : NULL);
        xa_thread_spawn_scan_note_local(scan, var ? var->name : NULL, class_name);
    } else if (node->type == AST_ASSIGNMENT) {
        AssignmentNode *assign = &node->as.assignment;
        const char *class_name =
            xa_thread_spawn_expr_sync_ctor_class(scan->ctx, assign ? assign->value : NULL);
        xa_thread_spawn_scan_note_local(scan, assign ? assign->name : NULL, class_name);
    }

    const char *feature = NULL;
    if (node->type == AST_AWAIT_EXPR)
        feature = "await";
    else if (node->type == AST_YIELD_STMT)
        feature = "yield";
    else if (node->type == AST_CALL_EXPR) {
        CallExprNode *call = &node->as.call_expr;
        const char *call_feature = NULL;
        if (xa_thread_spawn_call_is_sync_method(scan, call)) {
            feature = "coroutine-domain sync.* method";
        } else if (xa_thread_spawn_inline_call_may_suspend(scan->ctx, call, scan->call_stack,
                                                           scan->call_depth, &call_feature)) {
            feature = call_feature;
            if (!feature && call->callee && call->callee->type == AST_VARIABLE) {
                snprintf(scan->feature_buf, sizeof(scan->feature_buf),
                         "call to suspendable function '%s'",
                         call->callee->as.variable.name ? call->callee->as.variable.name : "?");
                feature = scan->feature_buf;
            } else if (!feature && call->callee && call->callee->type == AST_MEMBER_ACCESS) {
                MemberAccessNode *ma = &call->callee->as.member_access;
                if (ma->name && ma->object && ma->object->type == AST_VARIABLE &&
                    ma->object->as.variable.name) {
                    snprintf(scan->feature_buf, sizeof(scan->feature_buf),
                             "call to suspendable function '%s.%s'", ma->object->as.variable.name,
                             ma->name);
                    feature = scan->feature_buf;
                }
            }
        }
    }
    if (!feature)
        return;

    scan->found = true;
    if (scan->report)
        xa_report_thread_spawn_suspend(scan, node, feature);
}

static void xa_thread_spawn_sync_scan_post(AstNode *node, void *ud) {
    XaThreadSpawnSyncScan *scan = (XaThreadSpawnSyncScan *) ud;
    if (!scan || !node)
        return;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
         node->type == AST_METHOD_DECL) &&
        scan->nested_function_depth > 0) {
        scan->nested_function_depth--;
        return;
    }
    if (scan->nested_function_depth > 0)
        return;
    if (node->type == AST_BLOCK && scan->block_depth > 0) {
        xa_thread_spawn_scan_pop_block(scan);
        scan->block_depth--;
    }
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

static bool xa_thread_spawn_body_may_suspend(XaInferContext *ctx, AstNode *body,
                                             XaSymbol **call_stack, int call_depth) {
    if (!ctx || !body)
        return false;
    XaThreadSpawnSyncScan scan = {.ctx = ctx,
                                  .call_depth = call_depth,
                                  .nested_function_depth = 0,
                                  .report = false,
                                  .found = false,
                                  .reported = false};
    for (int i = 0; i < call_depth && i < 32; i++)
        scan.call_stack[i] = call_stack[i];
    xa_ast_walk(body, xa_thread_spawn_sync_scan_pre, xa_thread_spawn_sync_scan_post, &scan);
    return scan.found;
}

static AstNode *xa_threadlocal_current_body(XaInferContext *ctx) {
    if (!ctx || !ctx->analyzer)
        return NULL;
    for (XaScope *s = ctx->analyzer->current_scope; s; s = s->parent) {
        if (s->kind != XA_SCOPE_FUNCTION || !s->ast_node)
            continue;
        AstNode *fn = (AstNode *) s->ast_node;
        if (fn->type == AST_FUNCTION_DECL)
            return fn->as.function_decl.body;
        if (fn->type == AST_FUNCTION_EXPR)
            return fn->as.function_expr.body;
        if (fn->type == AST_METHOD_DECL)
            return fn->as.method_decl.body;
        return NULL;
    }
    if (ctx->block_cursor_depth > 0 && ctx->block_cursor_nodes[0])
        return ctx->block_cursor_nodes[0];
    return ctx->current_block_node;
}

static bool xa_path_is_sys_stdlib_module(const char *file) {
    if (!file)
        return false;
    const char *suffixes[] = {"stdlib/sys/sys.xr", "stdlib\\sys\\sys.xr",
                              "<embedded stdlib>/sys/sys.xr"};
    for (int i = 0; i < (int) (sizeof(suffixes) / sizeof(suffixes[0])); i++) {
        size_t flen = strlen(file);
        size_t slen = strlen(suffixes[i]);
        if (flen >= slen && strcmp(file + flen - slen, suffixes[i]) == 0)
            return true;
    }
    return false;
}

static bool xa_class_info_is_sys_threadlocal(XaInferContext *ctx, XrClassInfo *info) {
    if (!info || !xa_class_name_matches_mono_base(info->name, "ThreadLocal"))
        return false;
    if (xa_path_is_sys_stdlib_module(info->location.file))
        return true;
    if (!ctx || !ctx->analyzer)
        return false;
    for (int i = 0; i < info->method_count; i++) {
        XaSymbol *method = info->methods ? info->methods[i] : NULL;
        XaSymbolLinks *links = method ? xa_analyzer_get_links(ctx->analyzer, method) : NULL;
        if (links && xa_path_is_sys_stdlib_module(links->file_path))
            return true;
    }
    return false;
}

static bool xa_class_info_has_source(XaInferContext *ctx, XrClassInfo *info) {
    if (!info)
        return false;
    if (info->location.file)
        return true;
    if (!ctx || !ctx->analyzer)
        return false;
    for (int i = 0; i < info->method_count; i++) {
        XaSymbol *method = info->methods ? info->methods[i] : NULL;
        XaSymbolLinks *links = method ? xa_analyzer_get_links(ctx->analyzer, method) : NULL;
        if (links && links->file_path)
            return true;
    }
    return false;
}

static bool xa_symbol_is_sys_threadlocal_class(XaInferContext *ctx, XaSymbol *sym,
                                               const char *name) {
    if (!ctx || !ctx->analyzer || !sym || (sym->kind != XA_SYM_CLASS && sym->kind != XA_SYM_IMPORT))
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    const char *member_name =
        links && links->import_member_name ? links->import_member_name : (name ? name : sym->name);
    if (!xa_class_name_matches_mono_base(member_name, "ThreadLocal"))
        return false;
    if (links && links->module_name && strcmp(links->module_name, "sys") == 0)
        return true;
    if (links && xa_path_is_sys_stdlib_module(links->file_path))
        return true;
    return links && xa_class_info_is_sys_threadlocal(ctx, links->class_info);
}

static bool xa_module_alias_is(XaInferContext *ctx, const char *name, const char *module_name) {
    if (!ctx || !ctx->analyzer || !name || !module_name)
        return false;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
    if (!sym || sym->kind != XA_SYM_MODULE)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    const char *actual = links && links->module_name ? links->module_name : name;
    return actual && strcmp(actual, module_name) == 0;
}

static bool xa_type_is_sys_threadlocal(XaInferContext *ctx, XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && !XR_TYPE_IS_INSTANCE(type)))
        return false;
    const char *class_name = xr_type_get_class_name(type);
    if (!xa_class_name_matches_mono_base(class_name, "ThreadLocal"))
        return false;
    if (type->instance.class_ref) {
        if (xa_class_info_is_sys_threadlocal(ctx, type->instance.class_ref))
            return true;
        if (xa_class_info_has_source(ctx, type->instance.class_ref))
            return false;
    }
    XaSymbol *sym = xa_lookup_visible_class_symbol(ctx, class_name);
    if (sym)
        return xa_symbol_is_sys_threadlocal_class(ctx, sym, class_name);

    /* Module-qualified stdlib construction can leave only the class_name on
     * the value type. With no visible user class named ThreadLocal to collide
     * with, treat that erased instance as sys.ThreadLocal. */
    return true;
}

static bool xa_call_is_sys_threadlocal_constructor(XaInferContext *ctx, CallExprNode *call,
                                                   XrType *callee_type) {
    if (!ctx || !call)
        return false;
    AstNode *callee = call->callee;
    if (callee && callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &callee->as.member_access;
        if (xa_class_name_matches_mono_base(ma->name, "ThreadLocal") && ma->object &&
            ma->object->type == AST_VARIABLE &&
            xa_module_alias_is(ctx, ma->object->as.variable.name, "sys")) {
            return true;
        }
    } else if (callee && callee->type == AST_VARIABLE) {
        const char *name = callee->as.variable.name;
        XaSymbol *sym = name ? xa_lookup_visible_symbol(ctx, name) : NULL;
        if (xa_symbol_is_sys_threadlocal_class(ctx, sym, name))
            return true;
    }
    return xa_type_is_sys_threadlocal(ctx, callee_type);
}

static void xa_check_threadlocal_suspend_context(XaInferContext *ctx, AstNode *node,
                                                 XrType *receiver_type, const char *method_name) {
    if (!ctx || !node || ctx->os_thread_body_depth > 0 || !method_name)
        return;
    if (strcmp(method_name, "get") != 0 && strcmp(method_name, "set") != 0)
        return;
    if (!xa_type_is_sys_threadlocal(ctx, receiver_type))
        return;

    AstNode *body = xa_threadlocal_current_body(ctx);
    if (!body)
        return;
    XaSymbol *call_stack[32] = {0};
    if (!xa_thread_spawn_body_may_suspend(ctx, body, call_stack, 0))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    xa_analyzer_add_diagnostic(
        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE,
        "sys.ThreadLocal.get/set cannot be used in a coroutine or suspendable function; use it "
        "only in sys.Thread OS-thread code or in code with no suspend points",
        &loc);
}

static void xa_check_threadlocal_initializer(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                             XrType *callee_type) {
    if (!ctx || !node || !call || !xa_call_is_sys_threadlocal_constructor(ctx, call, callee_type))
        return;
    if (call->arg_count <= 0 || !call->arguments || !call->arguments[0])
        return;

    XaSymbol *call_stack[32] = {0};
    XaThreadSpawnFunctionValueStatus status =
        xa_thread_spawn_function_value_expr_status(ctx, call->arguments[0], call_stack, 0);
    if (status == XA_THREAD_SPAWN_FN_VALUE_SAFE)
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    const char *message =
        status == XA_THREAD_SPAWN_FN_VALUE_MAY_SUSPEND
            ? "sys.ThreadLocal initializer cannot be suspendable; it runs in OS-thread code"
            : "sys.ThreadLocal initializer must be a known non-suspendable function value";
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE, message,
                               &loc);
}

static void xa_check_thread_spawn_sync_body(XaInferContext *ctx, AstNode *body) {
    AstNode *inline_body = xa_thread_spawn_inline_body(body);
    AstNode *scan_root = inline_body ? inline_body : body;
    if (!scan_root)
        return;
    XaThreadSpawnSyncScan scan = {.ctx = ctx,
                                  .call_depth = 0,
                                  .nested_function_depth = 0,
                                  .report = true,
                                  .found = false,
                                  .reported = false};
    xa_ast_walk(scan_root, xa_thread_spawn_sync_scan_pre, xa_thread_spawn_sync_scan_post, &scan);
    if (scan.found || scan.reported || body->type == AST_CALL_EXPR || inline_body)
        return;

    XaThreadSpawnFunctionValueStatus status =
        xa_thread_spawn_function_value_expr_status(ctx, body, scan.call_stack, scan.call_depth);
    if (status == XA_THREAD_SPAWN_FN_VALUE_SAFE)
        return;
    const char *feature = "call through dynamic function value";
    if (status == XA_THREAD_SPAWN_FN_VALUE_MAY_SUSPEND) {
        if (body->type == AST_VARIABLE && body->as.variable.name) {
            snprintf(scan.feature_buf, sizeof(scan.feature_buf), "suspendable function value '%s'",
                     body->as.variable.name);
            feature = scan.feature_buf;
        } else if (body->type == AST_MEMBER_ACCESS) {
            MemberAccessNode *ma = &body->as.member_access;
            if (ma->name && ma->object && ma->object->type == AST_VARIABLE &&
                ma->object->as.variable.name) {
                snprintf(scan.feature_buf, sizeof(scan.feature_buf),
                         "suspendable function value '%s.%s'", ma->object->as.variable.name,
                         ma->name);
                feature = scan.feature_buf;
            } else {
                feature = "suspendable function value";
            }
        } else {
            feature = "suspendable function value";
        }
    }
    scan.found = true;
    xa_report_thread_spawn_suspend(&scan, body, feature);
}

static XrType *xa_visit_sys_thread_spawn_call(XaInferContext *ctx, AstNode *node,
                                              CallExprNode *call) {
    int body_index = xa_thread_spawn_body_arg_index(call);
    if (body_index < 0 || !call->arguments[body_index]) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "sys.Thread.spawn expects fn or (ThreadOptions, fn)", &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (body_index == 1 && call->arguments[0])
        xa_check_thread_spawn_options(ctx, call->arguments[0]);

    AstNode *body = call->arguments[body_index];
    int saved_os_thread_body_depth = ctx->os_thread_body_depth;
    ctx->os_thread_body_depth++;
    XrType *body_type = xa_visit_infer_expr(ctx, body);
    ctx->os_thread_body_depth = saved_os_thread_body_depth;
    if (body && body->type == AST_CALL_EXPR)
        xa_check_spawn_call_boundary_args(ctx, node, &body->as.call_expr);
    xa_check_thread_spawn_sync_body(ctx, body);

    XrType *result_type = xr_type_new_unit(NULL);
    if (body_type && XR_TYPE_IS_FUNCTION(body_type) && body_type->function.return_type)
        result_type = body_type->function.return_type;
    else if (body_type && !XR_TYPE_IS_FUNCTION(body_type))
        result_type = body_type;

    XrType *args[1] = {result_type ? result_type : xr_type_new_unknown(NULL)};
    return xr_type_new_generic_instance(ctx->analyzer->isolate, "Thread", NULL, args, 1);
}

static bool xa_type_is_byte_slice_view(XrType *type) {
    return xr_type_is_u8_slice(type);
}

static bool xa_call_is_byte_slice_typed_load(CallExprNode *call, XrType *receiver_type) {
    if (!call || !receiver_type || !xa_type_is_byte_slice_view(receiver_type) || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "load") == 0;
}

static bool xa_call_is_byte_slice_typed_store(CallExprNode *call, XrType *receiver_type) {
    if (!call || !receiver_type || !xa_type_is_byte_slice_view(receiver_type) || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "store") == 0;
}

static bool xa_call_is_byte_slice_reinterpret(CallExprNode *call, XrType *receiver_type) {
    if (!call || !receiver_type || !xa_type_is_byte_slice_view(receiver_type) || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "reinterpret") == 0;
}

static bool xa_type_is_supported_byte_slice_typed_scalar(XrType *type) {
    if (!type)
        return false;
    if (XR_TYPE_IS_INT(type)) {
        switch (type->scalar_rep) {
            case XR_NATIVE_I16:
            case XR_NATIVE_U16:
            case XR_NATIVE_I32:
            case XR_NATIVE_U32:
            case XR_NATIVE_I64:
            case XR_NATIVE_U64:
                return true;
            default:
                return false;
        }
    }
    return XR_TYPE_IS_FLOAT(type) &&
           (type->scalar_rep == XR_NATIVE_F32 || type->scalar_rep == XR_NATIVE_F64);
}

static XrType *xa_byte_slice_typed_type_arg(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                            const char *label, bool allow_signed) {
    if (!ctx || !call)
        return xr_type_new_error(NULL);
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s expects exactly one type argument", label);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    XrType *target = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    if (xa_reject_error_type_success_type(ctx->analyzer, target, "generic type argument", label,
                                          node ? node->line : 0, node ? node->column : 0))
        return xr_type_new_error(NULL);
    bool supported = allow_signed && xa_type_is_supported_byte_slice_typed_scalar(target);
    if (!supported) {
        char msg[192];
        snprintf(msg, sizeof(msg), "%s currently supports T = %s", label,
                 "i16, u16, i32, u32, i64, u64, f32 or f64");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    return target;
}

static XrType *xa_load_le_return_type(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                      const char *label, bool allow_signed) {
    return xa_byte_slice_typed_type_arg(ctx, node, call, label, allow_signed);
}

static bool xa_type_is_supported_mem_access(XrType *type) {
    if (!type || type->is_nullable)
        return false;
    if (XR_TYPE_IS_POINTER(type))
        return true;
    if (XR_TYPE_IS_FLOAT(type))
        return type->scalar_rep == XR_NATIVE_F32 || type->scalar_rep == XR_NATIVE_F64;
    if (!XR_TYPE_IS_INT(type))
        return false;
    switch (type->scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static const char *xa_mem_access_member(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || (strcmp(ma->name, "load") != 0 && strcmp(ma->name, "store") != 0) ||
        !xa_call_object_is_module(ctx, ma->object, "mem"))
        return NULL;
    return ma->name;
}

static bool xa_mem_access_endian_literal(AstNode *arg, int64_t *out_endian) {
    if (!arg)
        return false;
    const char *enum_name = NULL;
    const char *member_name = NULL;
    if (arg->type == AST_ENUM_ACCESS) {
        enum_name = arg->as.enum_access.enum_name;
        member_name = arg->as.enum_access.member_name;
    } else if (arg->type == AST_MEMBER_ACCESS && arg->as.member_access.object &&
               arg->as.member_access.object->type == AST_VARIABLE) {
        enum_name = arg->as.member_access.object->as.variable.name;
        member_name = arg->as.member_access.name;
    }
    if (!enum_name || strcmp(enum_name, "Endian") != 0 || !member_name)
        return false;
    int64_t endian = -1;
    if (strcmp(member_name, "Native") == 0)
        endian = XR_ENDIAN_NATIVE;
    else if (strcmp(member_name, "LE") == 0)
        endian = XR_ENDIAN_LE;
    else if (strcmp(member_name, "BE") == 0)
        endian = XR_ENDIAN_BE;
    if (endian < 0)
        return false;
    if (out_endian)
        *out_endian = endian;
    return true;
}

static bool xa_type_is_endian(XrType *type) {
    return type && XR_TYPE_IS_ENUM(type) && type->enum_type.enum_name &&
           strcmp(type->enum_type.enum_name, "Endian") == 0;
}

static XrType *xa_mem_access_return_type(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                         const char *member) {
    bool is_store = member && strcmp(member, "store") == 0;
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    char label[64];
    snprintf(label, sizeof(label), "mem.%s<T>()", member ? member : "?");

    if (ctx->unsafe_depth == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s must be inside an unsafe block", label);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
    }
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s expects exactly one type argument", label);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   msg, &loc);
        return is_store ? xr_type_new_unit(ctx->analyzer->isolate)
                        : xr_type_new_unknown(ctx->analyzer->isolate);
    }

    XrType *target = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    if (xa_reject_error_type_success_type(ctx->analyzer, target, "generic type argument", label,
                                          node ? node->line : 0, node ? node->column : 0))
        return xr_type_new_error(NULL);
    if (!xa_type_is_supported_mem_access(target)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%s supports T = i8, u8, i16, u16, i32, u32, i64, u64, "
                 "isize, usize, f32, f64, Ptr<U> or MutPtr<U>",
                 label);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
    }

    int min_args = is_store ? 3 : 1;
    int max_args = is_store ? 4 : 3;
    if (call->arg_count < min_args || call->arg_count > max_args) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s expects %s", label,
                 is_store ? "pointer, byte offset, value and optional endian"
                          : "pointer and optional byte offset and endian");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
    }

    XrType *ptr_type = NULL;
    if (call->arg_count > 0 && call->arguments && call->arguments[0])
        ptr_type = xa_visit_infer_expr(ctx, call->arguments[0]);
    if (!ptr_type || !xr_type_is_u8_pointer(ptr_type) || (is_store && !ptr_type->ptr_is_mut)) {
        char msg[224];
        snprintf(msg, sizeof(msg), "%s expects %s as its first argument, got '%s'", label,
                 is_store ? "MutPtr<u8>" : "Ptr<u8> or MutPtr<u8>",
                 ptr_type ? xr_type_to_string(ptr_type) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }

    int offset_index = is_store ? 1 : (call->arg_count >= 2 ? 1 : -1);
    if (offset_index >= 0 && offset_index < call->arg_count && call->arguments[offset_index]) {
        XrType *offset_type = xa_visit_infer_expr(ctx, call->arguments[offset_index]);
        if (!offset_type || !XR_TYPE_IS_INT(offset_type)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "%s expects an integer byte offset, got '%s'", label,
                     offset_type ? xr_type_to_string(offset_type) : "unknown");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
    }

    if (is_store && call->arg_count >= 3 && call->arguments[2]) {
        XrType *saved_expected = ctx->expected_type;
        ctx->expected_type = target;
        XrType *value_type = xa_visit_infer_expr(ctx, call->arguments[2]);
        ctx->expected_type = saved_expected;
        if (target && value_type && !XR_TYPE_IS_UNKNOWN(target) &&
            !XR_TYPE_IS_UNKNOWN(value_type) && !xa_typecheck_assignable(target, value_type) &&
            !xr_is_json_coercion(target, value_type)) {
            char msg[224];
            snprintf(msg, sizeof(msg), "%s value type '%s' is not assignable to '%s'", label,
                     xr_type_to_string(value_type), xr_type_to_string(target));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
    }

    int endian_index = is_store ? 3 : (call->arg_count >= 3 ? 2 : -1);
    if (endian_index >= 0 && endian_index < call->arg_count && call->arguments[endian_index]) {
        XrType *endian_type = xa_visit_infer_expr(ctx, call->arguments[endian_index]);
        int64_t endian = XR_ENDIAN_NATIVE;
        bool literal = xa_mem_access_endian_literal(call->arguments[endian_index], &endian);
        if (!xa_type_is_endian(endian_type)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "%s endian must have type Endian, got '%s'", label,
                     endian_type ? xr_type_to_string(endian_type) : "unknown");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        } else if (XR_TYPE_IS_POINTER(target) && (!literal || endian != XR_ENDIAN_NATIVE)) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "%s pointer values require the compile-time literal Endian.Native", label);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
        }
    }

    return is_store ? xr_type_new_unit(ctx->analyzer->isolate) : target;
}

static XrType *xa_byte_slice_reinterpret_return_type(XaInferContext *ctx, AstNode *node,
                                                     CallExprNode *call) {
    if (!ctx || !call)
        return xr_type_new_error(NULL);
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   "Slice<byte>.reinterpret<T>() expects exactly one type argument",
                                   &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    XrType *target = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    if (xa_reject_error_type_success_type(ctx->analyzer, target, "generic type argument",
                                          "Slice<byte>.reinterpret<T>()", node ? node->line : 0,
                                          node ? node->column : 0))
        return xr_type_new_error(NULL);
    uint32_t target_size = 0;
    uint32_t target_align = 0;
    if (!xr_type_has_static_layout(xa_analyzer_target_data_layout(ctx->analyzer), target,
                                   &target_size, &target_align) ||
        !xr_type_all_bit_patterns_valid(target)) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT,
                                   "Slice<byte>.reinterpret<T>() requires a statically laid out "
                                   "target for which every bit pattern is valid",
                                   &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    return xr_type_new_slice(ctx->analyzer->isolate, target);
}

static bool xa_call_object_is_module(XaInferContext *ctx, AstNode *object,
                                     const char *module_name) {
    const char *actual = xa_call_object_module_name(ctx, object);
    return actual && module_name && strcmp(actual, module_name) == 0;
}

static const char *xa_call_object_module_name(XaInferContext *ctx, AstNode *object) {
    if (!ctx || !ctx->analyzer || !object || object->type != AST_VARIABLE)
        return NULL;
    const char *name = object->as.variable.name;
    if (!name)
        return NULL;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
    if (!sym || sym->kind != XA_SYM_MODULE)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    return links ? links->module_name : NULL;
}

bool xa_call_is_builtin_module_member(XaInferContext *ctx, const CallExprNode *call,
                                      const char *module_name, const char *member_name) {
    if (!ctx || !ctx->analyzer || !call || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS || !module_name || !member_name)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    if (!member->name || strcmp(member->name, member_name) != 0 || !member->object ||
        member->object->type != AST_VARIABLE || !member->object->as.variable.name)
        return false;
    /* Resolved symbol, not spelling: a shadowing declaration resolves to itself. */
    return xa_symbol_is_builtin_module(
        ctx->analyzer, xa_resolve_variable_symbol(ctx, member->object), module_name);
}

static XrType *xa_visit_coro_local_constructor(XaInferContext *ctx, AstNode *node,
                                               CallExprNode *call, bool *handled) {
    if (handled)
        *handled = false;
    if (!xa_call_is_builtin_module_member(ctx, call, "Coro", "Local"))
        return NULL;
    if (handled)
        *handled = true;

    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (call->arg_count != 0) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   "Coro.Local<T>() expects no value arguments", &loc);
        for (int i = 0; i < call->arg_count; i++)
            xa_visit_infer_expr(ctx, call->arguments[i]);
    }

    XrType *value_type = NULL;
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   "Coro.Local<T>() expects exactly one type argument", &loc);
        value_type = xr_type_new_unknown(ctx->analyzer->isolate);
    } else {
        value_type = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
        if (xa_reject_error_type_success_type(ctx->analyzer, value_type, "generic type argument",
                                              "Coro.Local<T>()", node ? node->line : 0,
                                              node ? node->column : 0))
            value_type = xr_type_new_unknown(ctx->analyzer->isolate);
    }

    XrType *type_args[1] = {value_type ? value_type : xr_type_new_unknown(ctx->analyzer->isolate)};
    return xr_type_new_generic_instance(ctx->analyzer->isolate, "CoroLocal", NULL, type_args, 1);
}

static XrType *xa_visit_coro_pool_submit(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                         bool *handled) {
    if (handled)
        *handled = false;
    if (!xa_call_is_builtin_module_member(ctx, call, "CoroPool", "submit"))
        return NULL;
    if (handled)
        *handled = true;

    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (call->type_arg_count != 0) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   "CoroPool.submit infers T and takes no explicit type arguments",
                                   &loc);
    }
    if (call->arg_count != 1) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   "CoroPool.submit expects exactly one zero-argument function",
                                   &loc);
    }

    XrType *fn_type = NULL;
    if (call->arg_count > 0 && call->arguments && call->arguments[0]) {
        XrType *saved_expected = ctx->expected_type;
        ctx->expected_type = xr_type_new_function(
            ctx->analyzer->isolate, NULL, 0, xr_type_new_unknown(ctx->analyzer->isolate), false);
        fn_type = xa_visit_infer_expr(ctx, call->arguments[0]);
        ctx->expected_type = saved_expected;
    }
    for (int i = 1; i < call->arg_count; i++)
        xa_visit_infer_expr(ctx, call->arguments[i]);

    XrType *result_type = xr_type_new_unknown(ctx->analyzer->isolate);
    if (!fn_type || !XR_TYPE_IS_FUNCTION(fn_type)) {
        if (call->arg_count > 0) {
            char msg[224];
            snprintf(msg, sizeof(msg), "CoroPool.submit expects fn(): T, got '%s'",
                     fn_type ? xr_type_to_string(fn_type) : "unknown");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
    } else {
        if (fn_type->function.param_count != 0) {
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       "CoroPool.submit function must take no arguments", &loc);
        }
        if (fn_type->function.return_type)
            result_type = fn_type->function.return_type;
    }
    return xr_type_new_task(ctx->analyzer->isolate, result_type);
}

static bool xa_intrinsic_is_parallel_plan_method(XaIntrinsicId intrinsic_id) {
    const XaIntrinsicDesc *desc = xa_intrinsic_by_id(intrinsic_id);
    return desc && desc->family == XA_INTRINSIC_FAMILY_PARALLEL &&
           (desc->flags & XA_INTRINSIC_FLAG_PLAN_RECEIVER) != 0;
}

static bool xa_intrinsic_is_parallel_module_function(XaIntrinsicId intrinsic_id) {
    const XaIntrinsicDesc *desc = xa_intrinsic_by_id(intrinsic_id);
    return desc && desc->family == XA_INTRINSIC_FAMILY_PARALLEL &&
           (desc->flags & XA_INTRINSIC_FLAG_PLAN_RECEIVER) == 0;
}

static bool xa_class_info_is_parallel_plan(XaInferContext *ctx, XrClassInfo *info) {
    if (!info || !ctx || !ctx->analyzer)
        return false;
    for (int i = 0; i < info->method_count; i++) {
        XaSymbol *method = info->methods ? info->methods[i] : NULL;
        XaSymbolLinks *links = method ? xa_analyzer_get_links(ctx->analyzer, method) : NULL;
        if (links && xa_intrinsic_is_parallel_plan_method(links->intrinsic_id))
            return true;
    }
    return false;
}

static XaIntrinsicId xa_parallel_module_call_intrinsic(XaInferContext *ctx, AstNode *callee,
                                                       XaSymbolLinks *links) {
    if (links && xa_intrinsic_is_parallel_module_function(links->intrinsic_id))
        return links->intrinsic_id;
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS)
        return XA_INTRINSIC_NONE;

    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !xa_call_object_is_module(ctx, ma->object, "parallel"))
        return XA_INTRINSIC_NONE;
    char key[192];
    int key_len = snprintf(key, sizeof(key), "parallel.%s", ma->name);
    const XaIntrinsicDesc *desc =
        key_len > 0 && (size_t) key_len < sizeof(key) ? xa_intrinsic_by_key(key) : NULL;
    return desc && xa_intrinsic_is_parallel_module_function(desc->id) ? desc->id
                                                                      : XA_INTRINSIC_NONE;
}

static XaIntrinsicId xa_parallel_plan_call_intrinsic(XaInferContext *ctx, XrType *receiver_type,
                                                     const char *method_name) {
    if (!ctx || !ctx->analyzer || !receiver_type || !method_name ||
        !XR_TYPE_IS_INSTANCE(receiver_type))
        return XA_INTRINSIC_NONE;
    if (receiver_type->semantic_type_id == XA_SEMANTIC_TYPE_PARALLEL_PLAN) {
        char key[192];
        int key_len = snprintf(key, sizeof(key), "parallel.Plan.%s", method_name);
        const XaIntrinsicDesc *desc =
            key_len > 0 && (size_t) key_len < sizeof(key) ? xa_intrinsic_by_key(key) : NULL;
        return desc && xa_intrinsic_is_parallel_plan_method(desc->id) ? desc->id
                                                                      : XA_INTRINSIC_NONE;
    }
    XrClassInfo *info = receiver_type->instance.class_ref;
    XaSymbol *method = info ? xa_class_info_lookup_instance_member(info, method_name) : NULL;
    XaSymbolLinks *links = method ? xa_analyzer_get_links(ctx->analyzer, method) : NULL;
    return links && xa_intrinsic_is_parallel_plan_method(links->intrinsic_id) ? links->intrinsic_id
                                                                              : XA_INTRINSIC_NONE;
}

static void xa_record_parallel_call_plan(XaInferContext *ctx, AstNode *node,
                                         XaIntrinsicId intrinsic_id) {
    if (!ctx || !ctx->analyzer || !node || intrinsic_id == XA_INTRINSIC_NONE ||
        !ctx->analyzer->parallel_call_plan_table)
        return;
    XaParallelCallKind kind = xa_parallel_call_kind_from_intrinsic(intrinsic_id);
    if (kind == XA_PAR_CALL_NONE)
        return;
    XaParallelCallPlan plan = {
        .kind = kind,
        .intrinsic_id = intrinsic_id,
        .is_plan_method = xa_intrinsic_is_parallel_plan_method(intrinsic_id),
    };
    xa_parallel_call_plan_table_set(
        (XaParallelCallPlanTable *) ctx->analyzer->parallel_call_plan_table, node, &plan);
}

static AstNode *xa_call_unwrap_grouping(AstNode *node) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node;
}

static bool xa_parallel_options_callee_is_parallel(XaInferContext *ctx, AstNode *callee) {
    callee = xa_call_unwrap_grouping(callee);
    if (!ctx || !callee)
        return false;
    if (callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &callee->as.member_access;
        return ma->name && strcmp(ma->name, "Options") == 0 &&
               xa_call_object_is_module(ctx, ma->object, "parallel");
    }
    if (callee->type == AST_VARIABLE && callee->as.variable.name) {
        XaSymbol *sym = xa_lookup_visible_symbol(ctx, callee->as.variable.name);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;
        const char *member = links && links->import_member_name
                                 ? links->import_member_name
                                 : (sym ? sym->name : callee->as.variable.name);
        return sym && sym->is_imported && links && links->module_name &&
               strcmp(links->module_name, "parallel") == 0 && member &&
               strcmp(member, "Options") == 0;
    }
    return false;
}

static bool xa_parallel_options_workers_const(XaInferContext *ctx, AstNode *options_arg,
                                              int64_t *out_workers) {
    options_arg = xa_call_unwrap_grouping(options_arg);
    if (!ctx || !options_arg || options_arg->type != AST_CALL_EXPR || !out_workers)
        return false;
    CallExprNode *ctor = &options_arg->as.call_expr;
    if (!xa_parallel_options_callee_is_parallel(ctx, ctor->callee) || ctor->arg_count != 1 ||
        !ctor->arguments || !ctor->arguments[0])
        return false;
    const char *err = NULL;
    return xa_eval_const_int_expr(ctx->analyzer, ctor->arguments[0], out_workers, &err);
}

static int xa_parallel_options_arg_index(const char *member, int arg_count) {
    if (!member)
        return -1;
    if ((strcmp(member, "forEach") == 0 || strcmp(member, "map") == 0) && arg_count >= 3)
        return 2;
    if (strcmp(member, "mapInto") == 0 && arg_count >= 4)
        return 3;
    if (strcmp(member, "reduce") == 0 && arg_count >= 5)
        return 4;
    return -1;
}

static void xa_check_parallel_options_workers_const(XaInferContext *ctx, AstNode *node,
                                                    CallExprNode *call, const char *member) {
    int options_index = call ? xa_parallel_options_arg_index(member, call->arg_count) : -1;
    if (!ctx || !call || options_index < 0 || options_index >= call->arg_count)
        return;
    int64_t workers = 0;
    if (!xa_parallel_options_workers_const(ctx, call->arguments[options_index], &workers) ||
        workers >= 0)
        return;
    AstNode *loc_node = call->arguments[options_index];
    XrLocation loc = {.file = ctx->file_path,
                      .line = loc_node ? loc_node->line : node->line,
                      .column = loc_node ? loc_node->column : node->column};
    char msg[128];
    snprintf(msg, sizeof(msg), "parallel.Options.workers must be >= 0, got %" PRId64, workers);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static const char *xa_parallel_callback_label_for_arg(const char *member, int arg_index) {
    if (!member)
        return NULL;
    if (strcmp(member, "forEach") == 0)
        return arg_index == 1 ? "parallel.forEach callback" : NULL;
    if (strcmp(member, "map") == 0)
        return arg_index == 1 ? "parallel.map callback" : NULL;
    if (strcmp(member, "mapInto") == 0)
        return arg_index == 2 ? "parallel.mapInto callback" : NULL;
    if (strcmp(member, "reduce") == 0) {
        if (arg_index == 2)
            return "parallel.reduce body callback";
        if (arg_index == 3)
            return "parallel.reduce combine callback";
    }
    return NULL;
}

static const char *xa_parallel_plan_callback_label_for_arg(const char *member, int arg_index) {
    if (!member)
        return NULL;
    if (strcmp(member, "forEach") == 0)
        return arg_index == 1 ? "parallel.Plan.forEach callback" : NULL;
    if (strcmp(member, "map") == 0)
        return arg_index == 1 ? "parallel.Plan.map callback" : NULL;
    if (strcmp(member, "mapInto") == 0)
        return arg_index == 2 ? "parallel.Plan.mapInto callback" : NULL;
    if (strcmp(member, "reduce") == 0) {
        if (arg_index == 2)
            return "parallel.Plan.reduce body callback";
        if (arg_index == 3)
            return "parallel.Plan.reduce combine callback";
    }
    return NULL;
}

static const char *xa_parallel_callback_label_for_plan(const XaParallelCallPlan *plan,
                                                       int arg_index) {
    if (!plan)
        return NULL;
    const char *member = xa_parallel_call_kind_name(plan->kind);
    return plan->is_plan_method ? xa_parallel_plan_callback_label_for_arg(member, arg_index)
                                : xa_parallel_callback_label_for_arg(member, arg_index);
}

static XrType *xa_parallel_callable_type(XaInferContext *ctx, CallExprNode *call,
                                         XrType *receiver_type, const XaParallelCallPlan *plan) {
    if (!ctx || !ctx->analyzer || !call || !plan)
        return NULL;
    XrType *state_type = NULL;
    if (plan->is_plan_method) {
        if (!receiver_type || !XR_TYPE_IS_INSTANCE(receiver_type))
            return NULL;
        state_type = receiver_type->instance.type_arg_count > 0 && receiver_type->instance.type_args
                         ? receiver_type->instance.type_args[0]
                         : NULL;
        if (!state_type && receiver_type->instance.class_ref) {
            XaSymbol *states =
                xa_class_info_lookup_instance_member(receiver_type->instance.class_ref, "_states");
            XaSymbolLinks *state_links =
                states ? xa_analyzer_get_links(ctx->analyzer, states) : NULL;
            if (state_links && state_links->type && XR_TYPE_IS_ARRAY(state_links->type))
                state_type = state_links->type->container.element_type;
        }
        if (!state_type)
            state_type = xr_type_new_unknown(ctx->analyzer->isolate);
    }
    XrType *range_type = xr_type_new_named_instance(ctx->analyzer->isolate, "Range");
    XrType *int_type = xr_type_new_int(ctx->analyzer->isolate);
    XrType *unit_type = xr_type_new_unit(ctx->analyzer->isolate);
    XrType *unknown_type = xr_type_new_unknown(ctx->analyzer->isolate);
    XrType *options_type = xr_type_new_named_instance(ctx->analyzer->isolate, "Options");
    XrType *body_params[] = {state_type, int_type};
    XrType *combine_params[2] = {unknown_type, unknown_type};
    XrType *params[5] = {0};
    XrType *return_type = unit_type;
    int param_count = 0;
    int min_params = 0;
    int body_param_offset = plan->is_plan_method ? 0 : 1;
    int body_param_count = plan->is_plan_method ? 2 : 1;

    switch (plan->kind) {
        case XA_PAR_CALL_FOR_EACH:
            params[0] = range_type;
            params[1] =
                xr_type_new_function(ctx->analyzer->isolate, body_params + body_param_offset,
                                     body_param_count, unit_type, false);
            if (plan->is_plan_method) {
                param_count = 2;
                min_params = 2;
            } else {
                params[2] = options_type;
                param_count = 3;
                min_params = 2;
            }
            break;
        case XA_PAR_CALL_MAP:
            params[0] = range_type;
            params[1] =
                xr_type_new_function(ctx->analyzer->isolate, body_params + body_param_offset,
                                     body_param_count, unknown_type, false);
            return_type = xr_type_new_array(ctx->analyzer->isolate, unknown_type);
            if (plan->is_plan_method) {
                param_count = 2;
                min_params = 2;
            } else {
                params[2] = options_type;
                param_count = 3;
                min_params = 2;
            }
            break;
        case XA_PAR_CALL_MAP_INTO: {
            XrType *output_type = call->arg_count > 1
                                      ? xa_visit_infer_expr(ctx, call->arguments[1])
                                      : xr_type_new_array(ctx->analyzer->isolate, unknown_type);
            XrType *element_type = output_type && XR_TYPE_IS_ARRAY(output_type)
                                       ? output_type->container.element_type
                                       : unknown_type;
            params[0] = range_type;
            params[1] = output_type;
            params[2] =
                xr_type_new_function(ctx->analyzer->isolate, body_params + body_param_offset,
                                     body_param_count, element_type, false);
            return_type = unit_type;
            if (plan->is_plan_method) {
                param_count = 3;
                min_params = 3;
            } else {
                params[3] = options_type;
                param_count = 4;
                min_params = 3;
            }
            break;
        }
        case XA_PAR_CALL_REDUCE: {
            XrType *acc_type =
                call->arg_count > 1 ? xa_visit_infer_expr(ctx, call->arguments[1]) : unknown_type;
            body_params[0] = state_type;
            body_params[1] = int_type;
            combine_params[0] = acc_type;
            combine_params[1] = acc_type;
            params[0] = range_type;
            params[1] = acc_type;
            params[2] =
                xr_type_new_function(ctx->analyzer->isolate, body_params + body_param_offset,
                                     body_param_count, acc_type, false);
            params[3] =
                xr_type_new_function(ctx->analyzer->isolate, combine_params, 2, acc_type, false);
            return_type = acc_type;
            if (plan->is_plan_method) {
                param_count = 4;
                min_params = 4;
            } else {
                params[4] = options_type;
                param_count = 5;
                min_params = 4;
            }
            break;
        }
        case XA_PAR_CALL_NONE:
            return NULL;
    }
    XrType *callable =
        xr_type_new_function(ctx->analyzer->isolate, params, param_count, return_type, false);
    if (callable) {
        callable->function.min_params = min_params;
        if (plan->kind == XA_PAR_CALL_MAP_INTO)
            xr_type_function_set_param_mode(callable, 1, XR_PARAM_REF);
    }
    return callable;
}

static XrType *xa_visit_call_arg_with_parallel_context(XaInferContext *ctx, AstNode *arg_node,
                                                       const char *callback_label) {
    if (!ctx || !arg_node)
        return xr_type_new_error(NULL);
    const char *saved_pending = ctx->pending_parallel_callback_name;
    if (callback_label && arg_node->type == AST_FUNCTION_EXPR)
        ctx->pending_parallel_callback_name = callback_label;
    XrType *type = xa_visit_infer_expr(ctx, arg_node);
    ctx->pending_parallel_callback_name = saved_pending;
    return type;
}

static bool xa_mem_layout_member_name(const char *name) {
    return name && (strcmp(name, "sizeOf") == 0 || strcmp(name, "alignOf") == 0 ||
                    strcmp(name, "offsetOf") == 0);
}

static const char *xa_mem_pointer_constructor_member(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || (strcmp(ma->name, "ptr") != 0 && strcmp(ma->name, "mutPtr") != 0) ||
        !xa_call_object_is_module(ctx, ma->object, "mem"))
        return NULL;
    return ma->name;
}

static bool xa_mem_slice_call(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "slice") == 0 &&
           xa_call_object_is_module(ctx, ma->object, "mem");
}

static bool xa_mem_with_slice_mut_call(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "withSliceMut") == 0 &&
           xa_call_object_is_module(ctx, ma->object, "mem");
}

static bool xa_mem_assume_initialized_call(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "assumeInitialized") == 0 &&
           xa_call_object_is_module(ctx, ma->object, "mem");
}

static AstNode *xa_ffi_output_unwrap_expr(AstNode *node) {
    node = xa_call_unwrap_grouping(node);
    if (node && node->type == AST_UNSAFE_EXPR) {
        node = node->as.unsafe_expr.operand;
        if (node && node->type == AST_BLOCK && node->as.block.count > 0) {
            node = node->as.block.statements[node->as.block.count - 1];
            if (node && node->type == AST_EXPR_STMT)
                node = node->as.expr_stmt;
        }
        node = xa_call_unwrap_grouping(node);
    }
    return node;
}

static bool xa_ffi_output_type_query_matches(XaInferContext *ctx, AstNode *node, const char *member,
                                             XrType *target) {
    node = xa_ffi_output_unwrap_expr(node);
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS || call->arg_count != 0 ||
        call->type_arg_count != 1 || !call->type_args || !call->type_args[0])
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || strcmp(ma->name, member) != 0 ||
        !xa_call_object_is_module(ctx, ma->object, "mem"))
        return false;
    XrType *queried = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    return queried && target && xr_type_equals(queried, target);
}

static bool xa_ffi_output_allocation_matches(XaInferContext *ctx, AstNode *initializer,
                                             XrType *target) {
    AstNode *node = xa_ffi_output_unwrap_expr(initializer);
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS || call->arg_count != 2 ||
        !call->arguments || !call->arguments[0] || !call->arguments[1])
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    return ma->name && strcmp(ma->name, "allocAligned") == 0 &&
           xa_call_object_is_module(ctx, ma->object, "mem") &&
           xa_ffi_output_type_query_matches(ctx, call->arguments[0], "sizeOf", target) &&
           xa_ffi_output_type_query_matches(ctx, call->arguments[1], "alignOf", target);
}

static bool xa_ffi_output_is_buffer_borrow(AstNode *node, uint32_t symbol_id,
                                           const char *symbol_name) {
    node = xa_ffi_output_unwrap_expr(node);
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count != 0 || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    AstNode *object = xa_call_unwrap_grouping(ma->object);
    return ma->name && strcmp(ma->name, "borrowPtr") == 0 && object &&
           object->type == AST_VARIABLE &&
           ((symbol_id && object->as.variable.symbol_id == symbol_id) ||
            (!symbol_id && symbol_name && object->as.variable.name &&
             strcmp(symbol_name, object->as.variable.name) == 0));
}

static const XrNativeParamContract *xa_ffi_output_param_contract(const XrNativeSymbol *symbol,
                                                                 int slot) {
    if (!symbol || !symbol->contract.complete || slot < 0)
        return NULL;
    for (uint32_t i = 0; i < symbol->contract.param_count; i++) {
        const XrNativeParamContract *param = &symbol->contract.params[i];
        if (param->index == (uint32_t) slot)
            return param;
    }
    return NULL;
}

static const XrNativeSymbol *xa_ffi_output_native_call(XaInferContext *ctx, AstNode *expr,
                                                       uint32_t buffer_symbol_id,
                                                       const char *buffer_name) {
    AstNode *node = xa_ffi_output_unwrap_expr(expr);
    if (!node || node->type != AST_CALL_EXPR)
        return NULL;
    CallExprNode *call = &node->as.call_expr;
    const char *callee_name = NULL;
    if (call->callee && call->callee->type == AST_VARIABLE) {
        callee_name = call->callee->as.variable.name;
    } else if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        callee_name = call->callee->as.member_access.name;
    }
    const XrNativePackagePlan *plan = xr_compiler_session_native_package_plan(
        ctx && ctx->analyzer ? ctx->analyzer->compiler_session : NULL);
    if (!plan || !plan->valid || plan->audit_mode != XR_NATIVE_AUDIT_SHIPPING)
        return NULL;
    const XrNativeSymbol *symbol = xr_native_package_find_symbol(plan, callee_name);
    if (!symbol || !symbol->contract.complete)
        return NULL;
    for (int i = 0; i < call->arg_count; i++) {
        if (!xa_ffi_output_is_buffer_borrow(call->arguments ? call->arguments[i] : NULL,
                                            buffer_symbol_id, buffer_name))
            continue;
        const XrNativeParamContract *param = xa_ffi_output_param_contract(symbol, i);
        if (param && param->output == XR_NATIVE_OUTPUT_COMPLETE &&
            (param->access == XR_NATIVE_ACCESS_WRITE ||
             param->access == XR_NATIVE_ACCESS_READWRITE) &&
            param->escape == XR_NATIVE_ESCAPE_NOESCAPE)
            return symbol;
    }
    return NULL;
}

static bool xa_ffi_output_zero_literal(AstNode *node) {
    node = xa_call_unwrap_grouping(node);
    return node && node->type == AST_LITERAL_INT && node->as.literal.int_bits == 0;
}

static bool xa_ffi_output_status_variable(AstNode *node, uint32_t symbol_id,
                                          const char *symbol_name) {
    node = xa_call_unwrap_grouping(node);
    return node && node->type == AST_VARIABLE &&
           ((symbol_id && node->as.variable.symbol_id == symbol_id) ||
            (!symbol_id && symbol_name && node->as.variable.name &&
             strcmp(symbol_name, node->as.variable.name) == 0));
}

static bool xa_ffi_output_success_guard(AstNode *stmt, uint32_t status_symbol_id,
                                        const char *status_name) {
    if (!stmt || stmt->type != AST_IF_STMT || !stmt->as.if_stmt.condition)
        return false;
    AstNode *condition = xa_call_unwrap_grouping(stmt->as.if_stmt.condition);
    if (!condition || (condition->type != AST_BINARY_NE && condition->type != AST_BINARY_EQ))
        return false;
    AstNode *left = condition->as.binary.left;
    AstNode *right = condition->as.binary.right;
    bool comparison = (xa_ffi_output_status_variable(left, status_symbol_id, status_name) &&
                       xa_ffi_output_zero_literal(right)) ||
                      (xa_ffi_output_status_variable(right, status_symbol_id, status_name) &&
                       xa_ffi_output_zero_literal(left));
    if (!comparison)
        return false;
    if (condition->type == AST_BINARY_NE)
        return stmt->as.if_stmt.then_branch &&
               !xa_statement_can_fall_through(stmt->as.if_stmt.then_branch);
    return stmt->as.if_stmt.else_branch &&
           !xa_statement_can_fall_through(stmt->as.if_stmt.else_branch);
}

typedef struct XaFfiBufferUseScan {
    uint32_t symbol_id;
    const char *name;
    bool found;
} XaFfiBufferUseScan;

static void xa_ffi_buffer_use_scan_pre(AstNode *node, void *userdata) {
    XaFfiBufferUseScan *scan = (XaFfiBufferUseScan *) userdata;
    if (!scan || scan->found || !node || node->type != AST_VARIABLE)
        return;
    if ((scan->symbol_id && node->as.variable.symbol_id == scan->symbol_id) ||
        (!scan->symbol_id && scan->name && node->as.variable.name &&
         strcmp(scan->name, node->as.variable.name) == 0))
        scan->found = true;
}

static bool xa_ffi_output_statement_uses_buffer(AstNode *stmt, uint32_t symbol_id,
                                                const char *name) {
    XaFfiBufferUseScan scan = {.symbol_id = symbol_id, .name = name};
    xa_ast_walk(stmt, xa_ffi_buffer_use_scan_pre, NULL, &scan);
    return scan.found;
}

static bool xa_ffi_output_proof(XaInferContext *ctx, XaSymbol *buffer, XrType *target) {
    if (!ctx || !buffer || !target || !ctx->current_block_node || ctx->current_block_stmt_index < 0)
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (ctx->current_block_node->type == AST_BLOCK) {
        statements = ctx->current_block_node->as.block.statements;
        count = ctx->current_block_node->as.block.count;
    } else if (ctx->current_block_node->type == AST_PROGRAM) {
        statements = ctx->current_block_node->as.program.statements;
        count = ctx->current_block_node->as.program.count;
    }
    int limit = ctx->current_block_stmt_index < count ? ctx->current_block_stmt_index : count;
    bool allocation = false;
    int output_index = -1;
    uint32_t status_symbol_id = 0;
    const char *status_name = NULL;
    const XrNativeSymbol *native = NULL;
    for (int i = 0; i < limit; i++) {
        AstNode *stmt = statements ? statements[i] : NULL;
        if (!stmt)
            continue;
        if (stmt->type == AST_VAR_DECL) {
            VarDeclNode *var = &stmt->as.var_decl;
            bool is_buffer =
                (buffer->id && var->symbol_id == buffer->id) ||
                (!buffer->id && var->name && buffer->name && strcmp(var->name, buffer->name) == 0);
            if (is_buffer) {
                allocation = xa_ffi_output_allocation_matches(ctx, var->initializer, target);
                output_index = -1;
                native = NULL;
                continue;
            }
            if (allocation && var->initializer) {
                const XrNativeSymbol *candidate =
                    xa_ffi_output_native_call(ctx, var->initializer, buffer->id, buffer->name);
                if (candidate) {
                    native = candidate;
                    output_index = i;
                    status_symbol_id = var->symbol_id;
                    status_name = var->name;
                }
            }
        } else if (allocation && stmt->type == AST_EXPR_STMT) {
            const XrNativeSymbol *candidate =
                xa_ffi_output_native_call(ctx, stmt->as.expr_stmt, buffer->id, buffer->name);
            if (candidate) {
                native = candidate;
                output_index = i;
                status_symbol_id = 0;
                status_name = NULL;
            }
        }
    }
    if (!allocation || !native || output_index < 0 || !native->contract.failure)
        return false;

    bool success_proven = strcmp(native->contract.failure, "none") == 0;
    for (int i = output_index + 1; i < limit; i++) {
        AstNode *stmt = statements[i];
        if (!success_proven && strcmp(native->contract.failure, "status_nonzero") == 0 &&
            xa_ffi_output_success_guard(stmt, status_symbol_id, status_name)) {
            success_proven = true;
            continue;
        }
        if (xa_ffi_output_statement_uses_buffer(stmt, buffer->id, buffer->name))
            return false;
    }
    return success_proven;
}

static XrType *xa_mem_assume_initialized_return_type(XaInferContext *ctx, AstNode *node,
                                                     CallExprNode *call) {
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (ctx->unsafe_depth == 0)
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "mem.assumeInitialized<T>() must be inside an unsafe block",
                                   &loc);
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
            "mem.assumeInitialized<T>() expects exactly one explicit type argument", &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }
    XrType *target = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    uint32_t size = 0;
    uint32_t align = 0;
    if (!target ||
        !xr_type_has_static_layout(xa_analyzer_target_data_layout(ctx->analyzer), target, &size,
                                   &align) ||
        !xr_type_all_bit_patterns_valid(target) || xa_type_layout_has_flexible_tail(target) ||
        size == 0 || align == 0) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_CONSTRAINT,
            "mem.assumeInitialized<T>() requires a fixed-layout, reference-free type whose "
            "validity is closed by complete native output evidence",
            &loc);
        return target ? target : xr_type_new_unknown(ctx->analyzer->isolate);
    }
    if (call->arg_count != 1 || !call->arguments || !call->arguments[0]) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   "mem.assumeInitialized<T>() expects one `move Buffer` argument",
                                   &loc);
        return target;
    }
    AstNode *move = xa_call_unwrap_grouping(call->arguments[0]);
    if (!move || move->type != AST_MOVE_EXPR) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "mem.assumeInitialized<T>() must consume its Buffer with move",
                                   &loc);
        (void) xa_visit_infer_expr(ctx, call->arguments[0]);
        return target;
    }
    AstNode *source = xa_call_unwrap_grouping(move->as.move_expr.expr);
    XaSymbol *buffer = source && source->type == AST_VARIABLE
                           ? xa_lookup_visible_symbol(ctx, source->as.variable.name)
                           : NULL;
    XrType *buffer_type = xa_visit_infer_expr(ctx, call->arguments[0]);
    if (!buffer || !buffer_type || !xr_type_is_builtin_named_class(buffer_type, "Buffer")) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "mem.assumeInitialized<T>() expects `move` of a mem.Buffer",
                                   &loc);
        return target;
    }
    if (!xa_ffi_output_proof(ctx, buffer, target)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "E-FFI-OUTPUT-EVIDENCE: typed materialization requires exact allocAligned(sizeOf<T>(), "
            "alignOf<T>()) provenance, a shipping output=complete/noescape native contract, and "
            "a dominating checked success path",
            &loc);
    }
    return target;
}

typedef struct XaWithSliceMutRebindScan {
    uint32_t symbol_id;
    const char *name;
    bool found;
} XaWithSliceMutRebindScan;

static void xa_with_slice_mut_rebind_scan_pre(AstNode *node, void *userdata) {
    XaWithSliceMutRebindScan *scan = (XaWithSliceMutRebindScan *) userdata;
    if (!scan || scan->found || !node || node->type != AST_ASSIGNMENT)
        return;
    AssignmentNode *assign = &node->as.assignment;
    if ((scan->symbol_id && assign->symbol_id == scan->symbol_id) ||
        (!scan->symbol_id && scan->name && assign->name && strcmp(scan->name, assign->name) == 0))
        scan->found = true;
}

static bool xa_with_slice_mut_callback_rebinds_view(AstNode *callback) {
    while (callback && callback->type == AST_GROUPING)
        callback = callback->as.grouping;
    if (!callback || callback->type != AST_FUNCTION_EXPR ||
        callback->as.function_expr.param_count != 1 || !callback->as.function_expr.params ||
        !callback->as.function_expr.params[0] || !callback->as.function_expr.body)
        return false;
    XrParamNode *param = callback->as.function_expr.params[0];
    XaWithSliceMutRebindScan scan = {.symbol_id = param->symbol_id, .name = param->name};
    xa_ast_walk(callback->as.function_expr.body, xa_with_slice_mut_rebind_scan_pre, NULL, &scan);
    return scan.found;
}

static bool xa_layout_has_flexible_tail_depth(const XrAggregateLayout *layout, unsigned depth) {
    if (!layout || depth > 8)
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->is_flexible)
            return true;
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE && field->sub_layout &&
            xa_layout_has_flexible_tail_depth(field->sub_layout, depth + 1))
            return true;
    }
    return false;
}

static bool xa_type_layout_has_flexible_tail(const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        !type->instance.class_ref)
        return false;
    return xa_layout_has_flexible_tail_depth(type->instance.class_ref->struct_layout, 0);
}

static XrType *xa_mem_slice_return_type(XaInferContext *ctx, AstNode *node, CallExprNode *call) {
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (ctx->unsafe_depth == 0) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "mem.slice<T>() must be inside an unsafe block", &loc);
    }
    if (call->type_arg_count > 1) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   "mem.slice<T>() accepts at most one explicit type argument",
                                   &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }
    if (call->arg_count != 3 || !call->arguments || !call->arguments[0] || !call->arguments[1] ||
        !call->arguments[2]) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   "mem.slice<T>() expects (pointer, count, owner)", &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }

    XrType *source = xa_visit_infer_expr(ctx, call->arguments[0]);
    XrType *count = xa_visit_infer_expr(ctx, call->arguments[1]);
    (void) xa_visit_infer_expr(ctx, call->arguments[2]);
    XrType *target =
        call->type_arg_count == 1 && call->type_args && call->type_args[0]
            ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0])
            : (source && XR_TYPE_IS_POINTER(source) ? source->container.element_type
                                                    : xr_type_new_unknown(ctx->analyzer->isolate));
    if (xa_reject_error_type_success_type(ctx->analyzer, target, "generic type argument",
                                          "mem.slice", node ? node->line : 0,
                                          node ? node->column : 0))
        return xr_type_new_error(NULL);
    uint32_t size = 0;
    uint32_t align = 0;
    if (!target || XR_TYPE_IS_UNKNOWN(target) ||
        !xr_type_has_static_layout(xa_analyzer_target_data_layout(ctx->analyzer), target, &size,
                                   &align) ||
        xa_type_layout_has_flexible_tail(target) || size == 0 || size > INT16_MAX || align == 0 ||
        align > INT16_MAX) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "mem.slice<T>() requires T to have a verified static layout, got '%s'",
                 target ? xr_type_to_string(target) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
    }

    if (!source || !XR_TYPE_IS_POINTER(source)) {
        char msg[224];
        snprintf(msg, sizeof(msg), "mem.slice<T>() expects Ptr<U> or MutPtr<U>, got '%s'",
                 source ? xr_type_to_string(source) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }
    if (!count || !XR_TYPE_IS_INT(count)) {
        char msg[224];
        snprintf(msg, sizeof(msg), "mem.slice<T>() count must be an integer, got '%s'",
                 count ? xr_type_to_string(count) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }

    if (!target)
        target = xr_type_new_unknown(ctx->analyzer->isolate);
    XrType *result = xr_type_new_slice(ctx->analyzer->isolate, target);
    return xr_type_make_const(ctx->analyzer->isolate, result);
}

static XrType *xa_mem_with_slice_mut_return_type(XaInferContext *ctx, AstNode *node,
                                                 CallExprNode *call) {
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (ctx->unsafe_depth == 0) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "mem.withSliceMut<T>() must be inside an unsafe block", &loc);
    }
    if (call->type_arg_count > 1) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
            "mem.withSliceMut<T>() accepts at most one explicit type argument", &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }
    if (call->arg_count != 4 || !call->arguments || !call->arguments[0] || !call->arguments[1] ||
        !call->arguments[2] || !call->arguments[3]) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
            "mem.withSliceMut<T>() expects (mutPointer, count, ref guard, callback)", &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }

    XrType *source = xa_visit_infer_expr(ctx, call->arguments[0]);
    XrType *count = xa_visit_infer_expr(ctx, call->arguments[1]);
    XrType *guard = xa_visit_infer_expr(ctx, call->arguments[2]);
    XrType *target =
        call->type_arg_count == 1 && call->type_args && call->type_args[0]
            ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0])
            : (source && XR_TYPE_IS_POINTER(source) ? source->container.element_type
                                                    : xr_type_new_unknown(ctx->analyzer->isolate));
    uint32_t size = 0;
    uint32_t align = 0;
    if (!source || !XR_TYPE_IS_POINTER(source) || !source->ptr_is_mut) {
        char msg[224];
        snprintf(msg, sizeof(msg), "mem.withSliceMut<T>() expects MutPtr<U>, got '%s'",
                 source ? xr_type_to_string(source) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }
    if (!count || !XR_TYPE_IS_INT(count)) {
        char msg[224];
        snprintf(msg, sizeof(msg), "mem.withSliceMut<T>() count must be an integer, got '%s'",
                 count ? xr_type_to_string(count) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }
    if (!target || XR_TYPE_IS_UNKNOWN(target) ||
        !xr_type_has_static_layout(xa_analyzer_target_data_layout(ctx->analyzer), target, &size,
                                   &align) ||
        xa_type_layout_has_flexible_tail(target) || size == 0 || size > INT16_MAX || align == 0 ||
        align > INT16_MAX) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "mem.withSliceMut<T>() requires T to have a verified static layout, got '%s'",
                 target ? xr_type_to_string(target) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
    }

    XrCallArgAccess guard_access = xa_call_arg_access(call, 2);
    xa_check_arg_access_authorization(ctx, node, call->arguments[2], guard_access, 2, XR_PARAM_REF);
    xa_check_ref_argument_not_readonly(ctx, node, call->arguments[2], 2, XR_PARAM_REF);

    XrType *slice_type = xr_type_new_slice(
        ctx->analyzer->isolate, target ? target : xr_type_new_unknown(ctx->analyzer->isolate));
    XrType *callback_params[1] = {slice_type};
    XrType *expected_callback =
        xr_type_new_function(ctx->analyzer->isolate, callback_params, 1,
                             xr_type_new_unknown(ctx->analyzer->isolate), false);
    if (expected_callback)
        xr_type_function_set_param_mode(expected_callback, 0, XR_PARAM_REF);
    XrType *saved_expected = ctx->expected_type;
    ctx->expected_type = expected_callback;
    XrType *callback = xa_visit_infer_expr(ctx, call->arguments[3]);
    ctx->expected_type = saved_expected;
    if (!callback || callback->kind != XR_KIND_FUNCTION || callback->function.param_count != 1) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "mem.withSliceMut<T>() callback must have signature (ref Slice<T>) -> R, got '%s'",
                 callback ? xr_type_to_string(callback) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }
    XrType *callback_param = xr_type_function_param_type(callback, 0);
    if (xr_type_function_param_mode(callback, 0) != XR_PARAM_REF || !callback_param ||
        !XR_TYPE_IS_SLICE(callback_param) ||
        !xr_type_equals(callback_param->container.element_type, target)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "mem.withSliceMut<T>() callback parameter must be `ref Slice<T>`", &loc);
    }
    XrType *result = callback->function.return_type;
    if (result && XR_TYPE_IS_SLICE(result)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "mem.withSliceMut<T>() callback cannot return its borrowed Slice", &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    if (xa_with_slice_mut_callback_rebinds_view(call->arguments[3])) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
            "mem.withSliceMut<T>() callback may mutate elements but cannot rebind its temporary "
            "Slice descriptor",
            &loc);
    }
    (void) guard;
    return result ? result : xr_type_new_unknown(ctx->analyzer->isolate);
}

static XrType *xa_mem_pointer_constructor_return_type(XaInferContext *ctx, AstNode *node,
                                                      CallExprNode *call, const char *member) {
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "mem.%s<T>() expects exactly one type argument", member);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   msg, &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }
    if (call->arg_count != 1 || !call->arguments || !call->arguments[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "mem.%s<T>() expects exactly one address argument", member);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
        return xr_type_new_unknown(ctx->analyzer->isolate);
    }

    XrType *addr_type = xa_visit_infer_expr(ctx, call->arguments[0]);
    if (!addr_type || !XR_TYPE_IS_INT(addr_type)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "mem.%s<T>() expects an integer address, got '%s'", member,
                 addr_type ? xr_type_to_string(addr_type) : "unknown");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }

    XrType *pointee = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    if (xa_reject_error_type_success_type(ctx->analyzer, pointee, "generic type argument", member,
                                          node ? node->line : 0, node ? node->column : 0))
        return xr_type_new_error(NULL);
    if (!pointee)
        pointee = xr_type_new_unknown(ctx->analyzer->isolate);
    return xr_type_new_pointer(ctx->analyzer->isolate, pointee, strcmp(member, "mutPtr") == 0);
}

static const char *xa_mem_layout_call_member(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!xa_mem_layout_member_name(ma->name) || !xa_call_object_is_module(ctx, ma->object, "mem"))
        return NULL;
    return ma->name;
}

static XrType *xa_mem_layout_return_type(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                         const char *member) {
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
        char msg[160];
        snprintf(msg, sizeof(msg), "mem.%s<T>() expects exactly one type argument", member);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    bool is_offset = strcmp(member, "offsetOf") == 0;
    int expected_args = is_offset ? 1 : 0;
    if (call->arg_count != expected_args) {
        char msg[180];
        if (is_offset) {
            snprintf(msg, sizeof(msg),
                     "mem.offsetOf<T>() expects exactly one string literal field name");
        } else {
            snprintf(msg, sizeof(msg), "mem.%s<T>() expects no value arguments", member);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    XrType *target = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
    if (xa_reject_error_type_success_type(ctx->analyzer, target, "generic type argument", member,
                                          node ? node->line : 0, node ? node->column : 0))
        return xr_type_new_error(NULL);
    uint32_t size = 0;
    uint32_t align = 0;
    if (!xr_type_has_static_layout(xa_analyzer_target_data_layout(ctx->analyzer), target, &size,
                                   &align)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "mem.%s<T>() requires T to have a static C-compatible layout, got '%s'", member,
                 xr_type_to_string(target));
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (is_offset) {
        AstNode *field_arg = call->arguments[0];
        if (!field_arg || field_arg->type != AST_LITERAL_STRING ||
            !field_arg->as.literal.raw_value.string_val) {
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       "mem.offsetOf<T>() field name must be a string literal",
                                       &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
        const char *field = field_arg->as.literal.raw_value.string_val;
        uint32_t offset = 0;
        if (!xr_type_has_static_field_offset(xa_analyzer_target_data_layout(ctx->analyzer), target,
                                             field, &offset)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "mem.offsetOf<T>(): field '%s' not found in '%s'", field,
                     xr_type_to_string(target));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
    }

    (void) size;
    (void) align;
    return xr_type_new_int(ctx->analyzer->isolate);
}

static const char *xa_math_module_call_member(XaInferContext *ctx, CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || !xa_call_object_is_module(ctx, ma->object, "math"))
        return NULL;
    return ma->name;
}

static const char *xa_math_call_member(XaInferContext *ctx, CallExprNode *call,
                                       XaSymbolLinks *fn_links) {
    const char *member = xa_math_module_call_member(ctx, call);
    if (member)
        return member;
    if (!call || !call->callee || call->callee->type != AST_VARIABLE || !fn_links ||
        !fn_links->module_name || strcmp(fn_links->module_name, "math") != 0)
        return NULL;
    return fn_links->import_member_name ? fn_links->import_member_name
                                        : call->callee->as.variable.name;
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

static bool xa_freestanding_math_call_supported(const char *member, XrType **arg_types,
                                                int arg_count) {
    if (!member)
        return true;
    if ((strcmp(member, "min") == 0 || strcmp(member, "max") == 0) && arg_count > 0)
        return xa_all_args_are_int(arg_types, arg_count);
    if (strcmp(member, "clamp") == 0 && arg_count == 3)
        return xa_all_args_are_int(arg_types, arg_count);
    return false;
}

static void xa_check_freestanding_math_call(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                            XaSymbolLinks *fn_links, XrType **arg_types,
                                            int arg_count) {
    if (!ctx || !ctx->analyzer || !xa_freestanding_profile_enabled(ctx->analyzer))
        return;
    const char *member = xa_math_call_member(ctx, call, fn_links);
    if (!member || xa_freestanding_math_call_supported(member, arg_types, arg_count))
        return;
    char feature[192];
    snprintf(feature, sizeof(feature), "math.%s", member);
    xa_freestanding_report_unavailable(
        ctx, node, feature,
        "freestanding math currently allows literal constants and int-only min/max/clamp; "
        "libm-backed or floating math helpers are hosted-only");
}

static XrParamMode xa_call_param_mode(XrType *callee_type, int slot) {
    if (!callee_type || !XR_TYPE_IS_FUNCTION(callee_type) || slot < 0 ||
        slot >= callee_type->function.param_count)
        return XR_PARAM_READ;
    return xr_type_function_param_mode(callee_type, slot);
}

static const char *xa_call_param_mode_label(XrParamMode mode) {
    return xr_param_mode_label(mode);
}

#define XA_CALL_ALIAS_PATH_MAX 256

static XaSymbol *xa_call_variable_symbol(XaInferContext *ctx, AstNode *expr) {
    while (expr && expr->type == AST_GROUPING)
        expr = expr->as.grouping;
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    return xa_lookup_visible_symbol(ctx, expr->as.variable.name);
}

static bool xa_call_alias_path_copy(char *dst, size_t dst_size, const char *src);
static XaSymbol *xa_call_alias_path_symbol(XaInferContext *ctx, AstNode *expr, char *path,
                                           size_t path_size, bool *out_precise);

static XrType *xa_call_direct_variable_type_without_read(XaInferContext *ctx, AstNode *arg_node) {
    AstNode *place = xa_call_unwrap_grouping(arg_node);
    if (!ctx || !ctx->analyzer || !place || place->type != AST_VARIABLE)
        return NULL;
    XaSymbol *sym = xa_call_variable_symbol(ctx, place);
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

static XrType *xa_visit_call_arg_for_param_mode(XaInferContext *ctx, AstNode *arg_node,
                                                const char *callback_label, XrCallArgAccess access,
                                                XrParamMode param_mode) {
    (void) access;
    (void) param_mode;
    return xa_visit_call_arg_with_parallel_context(ctx, arg_node, callback_label);
}

static void xa_report_arg_access_requires_known_contract(XaInferContext *ctx, AstNode *call_node,
                                                         AstNode *arg_node, XrCallArgAccess access,
                                                         int slot) {
    if (!ctx || !ctx->analyzer || access == XR_CALL_ARG_PLAIN || slot < 0)
        return;
    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node    ? arg_node->line
                              : call_node ? call_node->line
                                          : 0,
                      .column = arg_node    ? arg_node->column
                                : call_node ? call_node->column
                                            : 0};
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Argument %d uses `%s`, but ref/move call authorization requires a known function "
             "parameter contract",
             slot + 1, xr_call_arg_access_label(access));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static void xa_report_arg_accesses_require_known_contract(XaInferContext *ctx, AstNode *call_node,
                                                          const CallExprNode *call) {
    if (!ctx || !call)
        return;
    for (int i = 0; i < call->arg_count; i++) {
        XrCallArgAccess access = xa_call_arg_access(call, i);
        if (access != XR_CALL_ARG_PLAIN)
            xa_report_arg_access_requires_known_contract(
                ctx, call_node, call->arguments ? call->arguments[i] : NULL, access, i);
    }
}

static void xa_report_arg_access_without_matching_contract(XaInferContext *ctx, AstNode *call_node,
                                                           AstNode *arg_node,
                                                           XrCallArgAccess access, int slot) {
    if (!ctx || !ctx->analyzer || access == XR_CALL_ARG_PLAIN || slot < 0)
        return;
    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node    ? arg_node->line
                              : call_node ? call_node->line
                                          : 0,
                      .column = arg_node    ? arg_node->column
                                : call_node ? call_node->column
                                            : 0};
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Argument %d uses `%s`, but no matching parameter exists in the known function "
             "parameter contract",
             slot + 1, xr_call_arg_access_label(access));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static bool xa_call_has_explicit_arg_access(const CallExprNode *call) {
    if (!call)
        return false;
    for (int i = 0; i < call->arg_count; i++) {
        if (xa_call_arg_access(call, i) != XR_CALL_ARG_PLAIN)
            return true;
    }
    return false;
}

static bool xa_call_slot_has_function_param_contract(XrType *callee_type, int slot) {
    if (!callee_type || !XR_TYPE_IS_FUNCTION(callee_type) || slot < 0)
        return false;
    if (slot < callee_type->function.param_count)
        return true;
    return callee_type->function.is_variadic && callee_type->function.param_count > 0;
}

static bool xa_call_alias_path_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src)
        return false;
    int n = snprintf(dst, dst_size, "%s", src);
    return n >= 0 && (size_t) n < dst_size;
}

static bool xa_call_alias_path_append(char *dst, size_t dst_size, const char *suffix) {
    if (!dst || dst_size == 0 || !suffix)
        return false;
    size_t len = strlen(dst);
    if (len >= dst_size)
        return false;
    int n = snprintf(dst + len, dst_size - len, "%s", suffix);
    return n >= 0 && (size_t) n < dst_size - len;
}

static bool xa_call_alias_index_segment(AstNode *index, char *buf, size_t buf_size) {
    if (!index || !buf || buf_size == 0)
        return false;
    while (index && (index->type == AST_GROUPING || index->type == AST_FORCE_UNWRAP))
        index = index->type == AST_GROUPING ? index->as.grouping : index->as.unary.operand;
    if (!index || index->type != AST_LITERAL_INT)
        return false;
    int n = snprintf(buf, buf_size, "[%lld]", (long long) index->as.literal.raw_value.int_val);
    return n >= 0 && (size_t) n < buf_size;
}

static XaSymbol *xa_call_root_variable_symbol(XaInferContext *ctx, AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
                return xa_call_variable_symbol(ctx, expr);
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                continue;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                continue;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                continue;
            case AST_GROUPING:
                expr = expr->as.grouping;
                continue;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                continue;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                continue;
            default:
                return NULL;
        }
    }
    return NULL;
}

static XaSymbol *xa_call_alias_path_symbol(XaInferContext *ctx, AstNode *expr, char *path,
                                           size_t path_size, bool *out_precise) {
    if (out_precise)
        *out_precise = true;
    if (!ctx || !ctx->analyzer || !expr || !path || path_size == 0)
        return NULL;
    path[0] = '\0';

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

    switch (expr->type) {
        case AST_VARIABLE: {
            XaSymbol *sym = xa_call_variable_symbol(ctx, expr);
            if (!sym || !sym->name || !xa_call_alias_path_copy(path, path_size, sym->name))
                return NULL;
            return sym;
        }
        case AST_MEMBER_ACCESS: {
            bool precise = true;
            XaSymbol *root = xa_call_alias_path_symbol(ctx, expr->as.member_access.object, path,
                                                       path_size, &precise);
            if (!root)
                return NULL;
            if (precise && expr->as.member_access.name) {
                if (!xa_call_alias_path_append(path, path_size, ".") ||
                    !xa_call_alias_path_append(path, path_size, expr->as.member_access.name))
                    precise = false;
            }
            if (out_precise)
                *out_precise = precise;
            return root;
        }
        case AST_INDEX_GET: {
            bool precise = true;
            XaSymbol *root =
                xa_call_alias_path_symbol(ctx, expr->as.index_get.array, path, path_size, &precise);
            if (!root)
                return NULL;
            char segment[64];
            if (precise &&
                xa_call_alias_index_segment(expr->as.index_get.index, segment, sizeof(segment))) {
                if (!xa_call_alias_path_append(path, path_size, segment))
                    precise = false;
            } else {
                xa_call_alias_path_copy(path, path_size, root->name ? root->name : "");
                precise = false;
            }
            if (out_precise)
                *out_precise = precise;
            return root;
        }
        case AST_SLICE_EXPR: {
            bool precise = true;
            XaSymbol *root = xa_call_alias_path_symbol(ctx, expr->as.slice_expr.source, path,
                                                       path_size, &precise);
            if (out_precise)
                *out_precise = precise;
            return root;
        }
        default:
            return NULL;
    }
}

static bool xa_call_alias_path_is_same_or_nested(const char *path, const char *prefix) {
    if (!path || !prefix)
        return true;
    size_t len = strlen(prefix);
    if (strncmp(path, prefix, len) != 0)
        return false;
    return path[len] == '\0' || path[len] == '.' || path[len] == '[';
}

static bool xa_call_alias_paths_may_overlap(const char *a, bool a_precise, const char *b,
                                            bool b_precise) {
    if (!a_precise || !b_precise || !a || !b || a[0] == '\0' || b[0] == '\0')
        return true;
    return xa_call_alias_path_is_same_or_nested(a, b) || xa_call_alias_path_is_same_or_nested(b, a);
}

static bool xa_method_call_creates_span_borrow(XaInferContext *ctx, XrType *receiver_type,
                                               const char *method_name,
                                               const XaSymbolLinks *method_links) {
    if (!ctx || !ctx->analyzer || !receiver_type || !method_name)
        return false;
    bool canonical_view_method =
        (xr_type_is_builtin_named_class(receiver_type, "Buffer") &&
         (strcmp(method_name, "asBytes") == 0 || strcmp(method_name, "asMutBytes") == 0)) ||
        (XR_TYPE_IS_SLICE(receiver_type) &&
         (strcmp(method_name, "asBytes") == 0 || strcmp(method_name, "reinterpret") == 0)) ||
        (XR_TYPE_IS_STRING(receiver_type) && strcmp(method_name, "bytes") == 0);
    if (canonical_view_method)
        return true;
    /* Native/user method metadata is the canonical source for non-container
     * view-returning methods such as Buffer.asBytes().  The builtin helper
     * below still covers compact container methods whose return type is
     * synthesized rather than represented by a method symbol. */
    if (method_links && method_links->return_type && XR_TYPE_IS_SLICE(method_links->return_type))
        return true;
    XrType *result =
        xa_builtin_get_method_return_type(ctx->analyzer->isolate, receiver_type, method_name);
    return result && XR_TYPE_IS_SLICE(result);
}

static bool xa_call_is_copy_builtin(const CallExprNode *call) {
    return call && call->arg_count == 1 && call->callee && call->callee->type == AST_VARIABLE &&
           call->callee->as.variable.name && strcmp(call->callee->as.variable.name, "copy") == 0;
}

static bool xa_expr_needs_contextual_view_type(AstNode *expr) {
    if (!expr)
        return false;
    if (expr->type == AST_SLICE_EXPR)
        return true;
    if (expr->type == AST_GROUPING)
        return xa_expr_needs_contextual_view_type(expr->as.grouping);
    if (expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const char *name = call->callee->as.member_access.name;
    return name && (strcmp(name, "bytes") == 0 || strcmp(name, "asBytes") == 0 ||
                    strcmp(name, "asMutBytes") == 0 || strcmp(name, "reinterpret") == 0);
}

static bool xa_expr_needs_parameter_context(AstNode *expr) {
    if (!expr)
        return false;
    if (expr->type == AST_GROUPING)
        return xa_expr_needs_parameter_context(expr->as.grouping);
    /* Numeric literals are typed by the resolved parameter contract.  The
     * eager symbol-resolution walk below must not first analyze them as the
     * default int/float, otherwise a valid u64-only magnitude leaves a stale
     * range diagnostic before the authoritative call-argument pass runs. */
    if (expr->type == AST_LITERAL_INT || expr->type == AST_LITERAL_FLOAT)
        return true;
    if (expr->type == AST_UNARY_NEG)
        return xa_expr_needs_parameter_context(expr->as.unary.operand);
    if (expr->type == AST_FUNCTION_EXPR || xa_expr_needs_contextual_view_type(expr))
        return true;
    /* Every array literal can be a fixed array when the parameter contract
     * supplies that layout.  Eagerly inferring only non-empty literals loses
     * the contract and misclassifies them as heap-backed Array<T>. */
    if (expr->type == AST_ARRAY_LITERAL)
        return true;
    if (expr->type == AST_MAP_LITERAL) {
        if (expr->as.map_literal.count == 0)
            return true;
        for (int i = 0; i < expr->as.map_literal.count; i++) {
            if (xa_expr_needs_parameter_context(expr->as.map_literal.keys[i]) ||
                xa_expr_needs_parameter_context(expr->as.map_literal.values[i]))
                return true;
        }
        return false;
    }
    if (expr->type == AST_SET_LITERAL) {
        if (expr->as.set_literal.count == 0)
            return true;
        for (int i = 0; i < expr->as.set_literal.count; i++) {
            if (xa_expr_needs_parameter_context(expr->as.set_literal.elements[i]))
                return true;
        }
        return false;
    }
    if (expr->type == AST_OBJECT_LITERAL) {
        for (int i = 0; i < expr->as.object_literal.count; i++) {
            if ((expr->as.object_literal.computed && expr->as.object_literal.computed[i] &&
                 xa_expr_needs_parameter_context(expr->as.object_literal.keys[i])) ||
                xa_expr_needs_parameter_context(expr->as.object_literal.values[i]))
                return true;
        }
        return false;
    }
    if (expr->type == AST_TUPLE_LITERAL) {
        for (int i = 0; i < expr->as.tuple_literal.count; i++) {
            if (xa_expr_needs_parameter_context(expr->as.tuple_literal.elements[i]))
                return true;
        }
        return false;
    }
    return false;
}

static bool xa_type_is_u8_view_or_owner(XrType *type) {
    return xr_type_is_u8_array(type) || xr_type_is_u8_slice(type);
}

static XrType *xa_copy_owned_return_type(XaInferContext *ctx, XrType *arg_type) {
    if (!ctx || !ctx->analyzer || !arg_type)
        return NULL;
    XrType *result = NULL;
    if (XR_TYPE_IS_SLICE(arg_type)) {
        if (xa_type_is_u8_view_or_owner(arg_type)) {
            result = xr_type_new_u8_array(ctx->analyzer->isolate);
        } else {
            XrType *elem = arg_type->container.element_type ? arg_type->container.element_type
                                                            : xr_type_new_unknown(NULL);
            result = xr_type_new_array(ctx->analyzer->isolate, elem);
        }
    } else {
        result = xr_type_copy(ctx->analyzer->isolate, arg_type);
    }
    if (result)
        result->is_const = false;
    return result;
}

static void xa_check_ref_argument_not_readonly(XaInferContext *ctx, AstNode *call_node,
                                               AstNode *arg_node, int slot, XrParamMode mode) {
    if (!ctx || !ctx->analyzer || mode != XR_PARAM_REF || !arg_node)
        return;
    XaSymbol *root = xa_call_root_variable_symbol(ctx, arg_node);
    if (!root || root->kind != XA_SYM_PARAMETER || root->passing_mode != XR_PARAM_READ)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node->line ? arg_node->line : call_node->line,
                      .column = arg_node->column ? arg_node->column : call_node->column};
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Cannot pass read parameter '%s' to ref parameter %d (readonly capability)",
             root->name ? root->name : "?", slot + 1);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN, msg,
                               &loc);
}

static XrCallArgAccess xa_call_arg_access(const CallExprNode *call, int index) {
    if (!call || index < 0 || index >= call->arg_count || !call->arg_accesses)
        return XR_CALL_ARG_PLAIN;
    XrCallArgAccess access = call->arg_accesses[index];
    return xr_call_arg_access_is_valid(access) ? access : XR_CALL_ARG_PLAIN;
}

/* Passing a binding as a `ref` argument hands the callee a writable place, so
 * every narrowing of that binding ends at the call and it reverts to its
 * declared type (spec §2.13 N-11.2). */
static void xa_invalidate_narrowing_for_ref_arg(XaInferContext *ctx, AstNode *arg_node) {
    if (!ctx || !ctx->flow || !arg_node)
        return;
    while (arg_node->type == AST_GROUPING)
        arg_node = arg_node->as.grouping;
    if (arg_node->type != AST_VARIABLE || !arg_node->as.variable.name)
        return;
    XaSymbol *sym = xa_lookup_visible_symbol(ctx, arg_node->as.variable.name);
    if (!sym || (sym->kind != XA_SYM_VARIABLE && sym->kind != XA_SYM_PARAMETER))
        return;
    XrType *declared = xa_analyzer_get_type(ctx->analyzer, sym);
    if (!declared || XR_TYPE_IS_UNKNOWN(declared))
        return;
    xa_flow_create_assignment(ctx->flow, NULL, arg_node->as.variable.name, declared);
}

static bool xa_call_arg_is_mutable_place(XaInferContext *ctx, AstNode *arg_node,
                                         const char **reason) {
    while (arg_node && arg_node->type == AST_GROUPING)
        arg_node = arg_node->as.grouping;
    if (!arg_node) {
        if (reason)
            *reason = "empty argument";
        return false;
    }

    switch (arg_node->type) {
        case AST_VARIABLE:
        case AST_MEMBER_ACCESS:
        case AST_INDEX_GET:
        case AST_SLICE_EXPR:
            break;
        default:
            if (reason)
                *reason = "temporary expression";
            return false;
    }

    XaSymbol *root = xa_call_root_variable_symbol(ctx, arg_node);
    if (root && (root->is_const || root->is_readonly_binding)) {
        if (reason)
            *reason = "readonly storage";
        return false;
    }
    if (root && xa_symbol_has_shared_provenance(root)) {
        if (reason)
            *reason = "readonly published storage";
        return false;
    }
    return true;
}

static const XaMemoryRootEffect *xa_call_memory_root_effect(const XaMemoryEffectSummary *summary,
                                                            XaMemoryRootKind kind, uint32_t index) {
    if (!summary)
        return NULL;
    for (uint32_t i = 0; i < summary->root_count; i++) {
        const XaMemoryRootEffect *effect = &summary->roots[i];
        if (effect->root.kind == kind && effect->root.index == index)
            return effect;
    }
    return NULL;
}

static void xa_check_call_memory_effect_actual(XaInferContext *ctx, AstNode *call_node,
                                               const XaMemoryEffectSummary *summary,
                                               XaMemoryRootKind kind, uint32_t index,
                                               AstNode *actual, const char *callee_name) {
    if (!ctx || !ctx->analyzer || !call_node || !actual)
        return;
    if (!summary)
        return;
    const XaMemoryRootEffect *effect = xa_call_memory_root_effect(summary, kind, index);
    bool incomplete = !xa_memory_effect_summary_is_complete(summary);
    AstNode *typed_actual = xa_call_unwrap_grouping(actual);
    if (typed_actual && typed_actual->type == AST_MOVE_EXPR)
        typed_actual = xa_call_unwrap_grouping(typed_actual->as.move_expr.expr);
    XrType *actual_type =
        typed_actual ? xa_analyzer_get_node_type(ctx->analyzer, typed_actual) : NULL;
    bool writes_through_slice = actual_type && actual_type->kind == XR_KIND_SLICE;
    bool conflicts =
        incomplete || (effect && ((!writes_through_slice && effect->descriptor_rebind) ||
                                  effect->relocation == XA_MEMORY_MAY_RELOCATE ||
                                  effect->shortening == XA_MEMORY_MAY_SHORTEN ||
                                  effect->invalidation == XA_MEMORY_INVALIDATES_VIEWS));
    if (!conflicts)
        return;

    char owner_path[512] = {0};
    XaSymbol *owner =
        xa_span_borrow_owner_path_for_owner_expr(ctx, actual, owner_path, sizeof(owner_path));
    if (!owner)
        owner = xa_span_borrow_owner_path_for_expr(ctx, actual, owner_path, sizeof(owner_path));
    if (!owner)
        return;

    char operation[256];
    if (incomplete) {
        snprintf(operation, sizeof(operation),
                 "calling '%s' with incomplete view-invalidation effects",
                 callee_name ? callee_name : "callee");
    } else if (effect->invalidation == XA_MEMORY_INVALIDATES_VIEWS) {
        snprintf(operation, sizeof(operation), "calling '%s' which invalidates views",
                 callee_name ? callee_name : "callee");
    } else if (effect->relocation == XA_MEMORY_MAY_RELOCATE) {
        snprintf(operation, sizeof(operation), "calling '%s' which may relocate the owner",
                 callee_name ? callee_name : "callee");
    } else if (effect->shortening == XA_MEMORY_MAY_SHORTEN) {
        snprintf(operation, sizeof(operation), "calling '%s' which may shorten the owner",
                 callee_name ? callee_name : "callee");
    } else {
        snprintf(operation, sizeof(operation), "calling mutating function '%s'",
                 callee_name ? callee_name : "callee");
    }
    xa_check_active_loan_owner_path_mutation(ctx, call_node, owner,
                                             owner_path[0] ? owner_path : NULL, operation);
}

XR_FUNC void xa_check_arg_access_authorization(XaInferContext *ctx, AstNode *call_node,
                                               AstNode *arg_node, XrCallArgAccess access, int slot,
                                               XrParamMode param_mode) {
    if (!ctx || !ctx->analyzer || slot < 0)
        return;
    if (access == XR_CALL_ARG_PLAIN) {
        if (param_mode == XR_PARAM_REF) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = arg_node ? arg_node->line : call_node->line,
                              .column = arg_node ? arg_node->column : call_node->column};
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "Argument %d must be passed as `%s` because parameter %d is %s", slot + 1,
                     xr_param_mode_label(param_mode), slot + 1, xr_param_mode_label(param_mode));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
        if (param_mode == XR_PARAM_MOVE) {
            AstNode *value = xa_call_unwrap_grouping(arg_node);
            bool fresh_or_copy = value && value->type != AST_VARIABLE &&
                                 value->type != AST_MEMBER_ACCESS && value->type != AST_INDEX_GET;
            if (!fresh_or_copy) {
                XrLocation loc = {.file = ctx->file_path,
                                  .line = arg_node ? arg_node->line : call_node->line,
                                  .column = arg_node ? arg_node->column : call_node->column};
                char msg[224];
                snprintf(msg, sizeof(msg),
                         "Argument %d passes an existing owner to move parameter %d without "
                         "transferring ownership; use `move`, `copy(...)`, or a fresh value",
                         slot + 1, slot + 1);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
            }
        }
        return;
    }

    XrParamMode required = access == XR_CALL_ARG_REF ? XR_PARAM_REF : XR_PARAM_MOVE;
    (void) 0;
    if (param_mode != required) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = arg_node ? arg_node->line : call_node->line,
                          .column = arg_node ? arg_node->column : call_node->column};
        char msg[192];
        snprintf(msg, sizeof(msg), "Argument %d uses `%s`, but parameter %d is %s", slot + 1,
                 xr_call_arg_access_label(access), slot + 1, xr_param_mode_label(param_mode));
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
        return;
    }

    if (access == XR_CALL_ARG_REF) {
        const char *reason = NULL;
        if (!xa_call_arg_is_mutable_place(ctx, arg_node, &reason)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = arg_node ? arg_node->line : call_node->line,
                              .column = arg_node ? arg_node->column : call_node->column};
            char msg[192];
            snprintf(msg, sizeof(msg), "Argument %d passed as `ref` must be a mutable place%s%s",
                     slot + 1, reason ? ": " : "", reason ? reason : "");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
        /* The callee may write through the place, so any narrowing of the
         * binding ends here and it reverts to its declared type
         * (spec §2.13 N-11.2). */
        xa_invalidate_narrowing_for_ref_arg(ctx, arg_node);
    }
}

static void xa_check_call_arg_access_authorization(XaInferContext *ctx, AstNode *call_node,
                                                   const CallExprNode *call, AstNode *arg_node,
                                                   int arg_index, int slot,
                                                   XrParamMode param_mode) {
    if (!call)
        return;
    xa_check_arg_access_authorization(ctx, call_node, arg_node, xa_call_arg_access(call, arg_index),
                                      slot, param_mode);
}

static void xa_check_ref_argument_aliases(XaInferContext *ctx, AstNode *call_node,
                                          uint32_t *arg_symbol_ids, const char **arg_names,
                                          XrParamMode *arg_modes,
                                          char (*arg_paths)[XA_CALL_ALIAS_PATH_MAX],
                                          bool *arg_path_precise, int arg_count) {
    if (!ctx || !ctx->analyzer || !arg_symbol_ids || !arg_names || !arg_modes)
        return;
    for (int i = 0; i < arg_count; i++) {
        if (arg_symbol_ids[i] == 0)
            continue;
        for (int j = i + 1; j < arg_count; j++) {
            if (arg_symbol_ids[j] == 0)
                continue;
            if (arg_symbol_ids[i] != arg_symbol_ids[j])
                continue;
            if (arg_modes[i] == XR_PARAM_READ && arg_modes[j] == XR_PARAM_READ)
                continue;
            if (arg_paths && arg_path_precise &&
                !xa_call_alias_paths_may_overlap(arg_paths[i], arg_path_precise[i], arg_paths[j],
                                                 arg_path_precise[j]))
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
    if (xr_type_is_builtin_named_class(receiver_type, "WorkQueue"))
        return strcmp(method_name, "push") == 0 && slot == 0;
    return false;
}

static void xa_check_borrowed_mutator_arg_escape(XaInferContext *ctx, AstNode *call_node,
                                                 XrType *receiver_type, const char *method_name,
                                                 AstNode *arg_node, XrType *arg_type, int slot) {
    if (!ctx || !call_node || !arg_node ||
        !xa_method_stores_argument(receiver_type, method_name, slot))
        return;
    if (arg_type && XR_TYPE_IS_POINTER(arg_type)) {
        char context[160];
        snprintf(context, sizeof(context), "store raw pointer borrow through method '%s'",
                 method_name ? method_name : "?");
        xa_check_pointer_borrow_escape(ctx, arg_node, arg_node, arg_type, context);
        return;
    }
    if (xa_type_contains_span_view(arg_type)) {
        char context[160];
        snprintf(context, sizeof(context), "pass Slice view to mutating method '%s'",
                 method_name ? method_name : "?");
        xa_check_span_value_escape(ctx, arg_node, arg_type, context);
        return;
    }
    xa_note_owner_escapes_into_heap(ctx, arg_node);
    if (!xa_type_needs_borrow_escape_guard(arg_type))
        return;
    XaSymbol *borrowed_root = xa_borrowed_param_root_symbol(ctx, arg_node);
    if (!borrowed_root || borrowed_root->passing_mode != XR_PARAM_REF)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node->line ? arg_node->line : call_node->line,
                      .column = arg_node->column ? arg_node->column : call_node->column};
    const char *name = borrowed_root->name ? borrowed_root->name : "?";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot pass borrowed '%s' parameter '%s' to mutating method '%s'; pass an owned "
             "value or copy(%s)",
             "ref", name, method_name ? method_name : "?", name);
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

static bool xa_sync_runtime_class_name(const char *name) {
    return name && (strcmp(name, "Semaphore") == 0 || strcmp(name, "CountdownLatch") == 0 ||
                    strcmp(name, "EventCount") == 0 || strcmp(name, "WorkQueue") == 0 ||
                    strcmp(name, "ResultGroup") == 0);
}

static bool xa_symbol_is_sync_runtime_class(XaInferContext *ctx, XaSymbol *sym, const char *name) {
    if (!ctx || !sym || (sym->kind != XA_SYM_CLASS && sym->kind != XA_SYM_IMPORT))
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links)
        return false;
    const char *class_name = links->import_member_name ? links->import_member_name : name;
    if (!xa_sync_runtime_class_name(class_name))
        return false;
    if (links->module_name && strcmp(links->module_name, "sync") == 0)
        return true;
    return xa_thread_spawn_path_is_sync_module(links->file_path);
}

static XrType *xa_sync_runtime_construct_type(XaInferContext *ctx, const char *name,
                                              CallExprNode *call) {
    if (!ctx || !name || !xa_sync_runtime_class_name(name))
        return NULL;
    if (strcmp(name, "WorkQueue") == 0) {
        XrType *elem = NULL;
        if (call && call->type_arg_count > 0 && call->type_args[0])
            elem = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
        if (!elem)
            elem = xr_type_new_unknown(NULL);
        XrType *args[1] = {elem};
        return xr_type_new_generic_instance(ctx->analyzer->isolate, "WorkQueue", NULL, args, 1);
    }
    return xr_type_new_named_instance(ctx->analyzer->isolate, name);
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
static XrType *xa_module_member_class_instance_type(XaInferContext *ctx, CallExprNode *call) {
    AstNode *callee = call ? call->callee : NULL;
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
    char semantic_key[192];
    int semantic_key_len =
        snprintf(semantic_key, sizeof(semantic_key), "%s.%s", mod_name, ma->name);
    XaSemanticTypeId semantic_type_id = (XaSemanticTypeId) call->semantic_type_id;
    if (semantic_type_id == XA_SEMANTIC_TYPE_NONE && !is_quoted && semantic_key_len > 0 &&
        (size_t) semantic_key_len < sizeof(semantic_key)) {
        semantic_type_id = xa_semantic_type_by_key(semantic_key);
        if (semantic_type_id != XA_SEMANTIC_TYPE_NONE) {
            /* Preserve canonical ownership across generic specialization. The
             * monomorphizer substitutes semantic_type_args while it may rewrite
             * the source spelling and consume ordinary call type arguments. */
            call->semantic_type_id = (uint32_t) semantic_type_id;
            call->semantic_type_args = call->type_args;
            call->semantic_type_arg_count = call->type_arg_count;
        }
    }
    XrTypeRef **semantic_trefs =
        call->semantic_type_id != 0 ? call->semantic_type_args : call->type_args;
    int semantic_tref_count =
        call->semantic_type_id != 0 ? call->semantic_type_arg_count : call->type_arg_count;
    const char *semantic_source_name = xa_semantic_type_source_name(semantic_type_id);
    XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, mod_name, is_quoted);
    if (!exports) {
        if (semantic_type_id == XA_SEMANTIC_TYPE_NONE)
            return NULL;
        int type_arg_count = semantic_tref_count;
        XrType **type_args =
            type_arg_count > 0 ? xr_malloc(sizeof(XrType *) * (size_t) type_arg_count) : NULL;
        if (type_arg_count > 0 && !type_args)
            return NULL;
        for (int i = 0; i < type_arg_count; i++)
            type_args[i] = xr_tref_resolve_in_analyzer(ctx->analyzer, semantic_trefs[i]);
        XrType *instance = xr_type_new_generic_instance(
            ctx->analyzer->isolate, semantic_source_name ? semantic_source_name : ma->name, NULL,
            type_args, type_arg_count);
        if (semantic_type_id == XA_SEMANTIC_TYPE_PARALLEL_PLAN && call->arg_count > 1 &&
            type_arg_count > 0 && type_args[0]) {
            XrType *lane_params[] = {xr_type_new_int(ctx->analyzer->isolate)};
            XrType *init_type =
                xr_type_new_function(ctx->analyzer->isolate, lane_params, 1, type_args[0], false);
            XrType *saved_expected = ctx->expected_type;
            ctx->expected_type = init_type;
            xa_visit_infer_expr(ctx, call->arguments[1]);
            ctx->expected_type = saved_expected;
        }
        xr_free(type_args);
        if (instance)
            instance->semantic_type_id = (uint32_t) semantic_type_id;
        return instance;
    }

    XaSymbol *member_sym = (XaSymbol *) xr_hashmap_get(exports, ma->name);
    if (!member_sym || member_sym->kind != XA_SYM_CLASS)
        return NULL;

    XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member_sym);
    if (member_links && member_links->class_info) {
        /* Module-member construction is an identity-preserving import edge.
         * Publish the canonical module and its sealed type capabilities on the
         * exported class symbol before constructing the instance.  This is
         * needed for embedded stdlib graphs, whose class locations are not
         * filesystem paths, and deliberately does not trust source spelling. */
        if (!is_quoted) {
            member_links->module_name = mod_name;
            member_links->class_info->capability_flags |=
                xa_stdlib_type_capability_flags(mod_name, ma->name);
        }
        XrType *instance = xa_class_constructor_instance_type(
            ctx, callee, call, ma->name, member_links, member_links->class_info);
        if (instance && semantic_type_id != XA_SEMANTIC_TYPE_NONE) {
            if (semantic_tref_count > 0) {
                XrType **type_args = xr_malloc(sizeof(XrType *) * (size_t) semantic_tref_count);
                if (!type_args)
                    return NULL;
                for (int i = 0; i < semantic_tref_count; i++)
                    type_args[i] = xr_tref_resolve_in_analyzer(ctx->analyzer, semantic_trefs[i]);
                instance = xr_type_new_generic_instance(
                    ctx->analyzer->isolate, semantic_source_name ? semantic_source_name : ma->name,
                    member_links->class_info, type_args, semantic_tref_count);
                xr_free(type_args);
            }
            instance->semantic_type_id = (uint32_t) semantic_type_id;
        }
        return instance;
    }
    if (!is_quoted && strcmp(mod_name, "sync") == 0)
        return xa_sync_runtime_construct_type(ctx, ma->name, call);
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

static XaSymbol *xa_module_member_class_symbol(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || node->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &node->as.member_access;
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
    return member_sym && member_sym->kind == XA_SYM_CLASS ? member_sym : NULL;
}

static XaSymbol *xa_static_method_class_symbol(XaInferContext *ctx, AstNode *class_expr) {
    if (!ctx || !class_expr)
        return NULL;
    if (class_expr->type == AST_VARIABLE)
        return xa_lookup_visible_class_symbol(ctx, class_expr->as.variable.name);
    return xa_module_member_class_symbol(ctx, class_expr);
}

static XaSymbolLinks *xa_static_method_fn_links(XaInferContext *ctx, AstNode *callee) {
    if (!ctx || !ctx->analyzer || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object)
        return NULL;
    XaSymbol *class_sym = xa_static_method_class_symbol(ctx, ma->object);
    XaSymbolLinks *class_links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
    XaSymbol *method_sym =
        (class_links && class_links->class_info)
            ? xa_class_info_lookup_static_member(class_links->class_info, ma->name)
            : NULL;
    if (!method_sym || method_sym->kind != XA_SYM_METHOD || !method_sym->is_static)
        return NULL;
    XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
    if (method_links && !method_links->file_path && class_links && class_links->class_info)
        method_links->file_path = class_links->class_info->location.file;
    return method_links;
}

static XaSymbolLinks *xa_static_method_fn_links_from_type(XaInferContext *ctx, XrType *class_type,
                                                          const char *method_name) {
    if (!ctx || !ctx->analyzer || !class_type || class_type->kind != XR_KIND_CLASS || !method_name)
        return NULL;
    XrClassInfo *class_info = class_type->instance.class_ref;
    if (!class_info && class_type->instance.class_name) {
        XaSymbol *class_sym = xa_lookup_visible_class_symbol(ctx, class_type->instance.class_name);
        XaSymbolLinks *class_links =
            class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
        class_info = class_links ? class_links->class_info : NULL;
    }
    XaSymbol *method_sym =
        class_info ? xa_class_info_lookup_static_member(class_info, method_name) : NULL;
    if (!method_sym || method_sym->kind != XA_SYM_METHOD || !method_sym->is_static)
        return NULL;
    XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
    if (method_links && !method_links->file_path && class_info)
        method_links->file_path = class_info->location.file;
    return method_links;
}

static XaSymbolLinks *xa_class_constructor_links(XaInferContext *ctx, XaSymbol *class_sym) {
    if (!ctx || !ctx->analyzer || !class_sym || class_sym->kind != XA_SYM_CLASS)
        return NULL;
    XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
    XrClassInfo *class_info = class_links ? class_links->class_info : NULL;
    XaSymbol *ctor =
        class_info ? xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR) : NULL;
    XaSymbolLinks *ctor_links = ctor ? xa_analyzer_get_links(ctx->analyzer, ctor) : NULL;
    if (ctor_links && !ctor_links->file_path && class_info)
        ctor_links->file_path = class_info->location.file;
    return ctor_links;
}

static bool xa_default_arg_path_matches(const char *a, const char *b) {
    if (!a || !b)
        return false;
    if (strcmp(a, b) == 0)
        return true;

    size_t alen = strlen(a);
    size_t blen = strlen(b);
    if (alen > blen && a[alen - blen - 1] == '/' && strcmp(a + alen - blen, b) == 0)
        return true;
    if (blen > alen && b[blen - alen - 1] == '/' && strcmp(b + blen - alen, a) == 0)
        return true;

    char *areal = xr_realpath(a);
    char *breal = xr_realpath(b);
    bool same = areal && breal && strcmp(areal, breal) == 0;
    if (areal)
        xr_free(areal);
    if (breal)
        xr_free(breal);
    return same;
}

static bool xa_default_arg_stdlib_module_from_path(const char *path, char *out, size_t out_cap) {
    if (!path || !out || out_cap == 0)
        return false;

    const char *marker = "stdlib/";
    const char *base = strstr(path, marker);
    if (!base)
        return false;
    base += strlen(marker);

    const char *slash = strchr(base, '/');
    if (!slash || slash == base)
        return false;

    size_t name_len = (size_t) (slash - base);
    if (name_len + 1 > out_cap)
        return false;

    const char *leaf = slash + 1;
    if (strncmp(leaf, base, name_len) != 0 || strcmp(leaf + name_len, ".xr") != 0)
        return false;

    memcpy(out, base, name_len);
    out[name_len] = '\0';
    return true;
}

static XrHashMap *xa_default_arg_decl_exports(XaInferContext *ctx, XaSymbolLinks *links) {
    if (!ctx || !ctx->analyzer || !links || !ctx->analyzer->graph)
        return NULL;

    if (links->module_name) {
        bool is_quoted = links->module_name[0] == '.' || links->module_name[0] == '/';
        XrHashMap *exports =
            resolve_graph_export_symbols(ctx->analyzer, links->module_name, is_quoted);
        if (exports)
            return exports;
    }

    if (!links->file_path)
        return NULL;

    XrModuleGraph *graph = (XrModuleGraph *) ctx->analyzer->graph;
    for (int i = 0; i < graph->spec_count; i++) {
        XrModuleSpec *spec = &graph->specs[i];
        if (!spec->export_symbols)
            continue;
        if ((spec->source_path &&
             xa_default_arg_path_matches(spec->source_path, links->file_path)) ||
            (spec->canonical && xa_default_arg_path_matches(spec->canonical, links->file_path)))
            return spec->export_symbols;
    }

    char module_name[64];
    if (xa_default_arg_stdlib_module_from_path(links->file_path, module_name,
                                               sizeof(module_name))) {
        return resolve_graph_export_symbols(ctx->analyzer, module_name, false);
    }
    return NULL;
}

static bool xa_default_arg_symbol_is_from_decl_file(XaAnalyzer *analyzer, XaSymbol *sym,
                                                    const char *decl_file, const char *name) {
    if (!sym || !decl_file || !name || !sym->name || strcmp(sym->name, name) != 0)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return links && links->file_path && xa_default_arg_path_matches(links->file_path, decl_file);
}

static XaSymbol *xa_default_arg_find_decl_file_symbol(XaAnalyzer *analyzer, XaScope *scope,
                                                      const char *decl_file, const char *name) {
    if (!analyzer || !scope || !decl_file || !name)
        return NULL;

    XaSymbol *local = xa_scope_lookup_local(scope, name);
    if (xa_default_arg_symbol_is_from_decl_file(analyzer, local, decl_file, name))
        return local;

    for (int i = 0; i < scope->child_count; i++) {
        XaSymbol *found =
            xa_default_arg_find_decl_file_symbol(analyzer, scope->children[i], decl_file, name);
        if (found)
            return found;
    }
    return NULL;
}

typedef struct XaDefaultArgBindCtx {
    XaInferContext *ctx;
    XrHashMap *exports;
    const char *decl_file;
} XaDefaultArgBindCtx;

static XaSymbol *xa_default_arg_lookup_decl_symbol(XaDefaultArgBindCtx *bind, const char *name) {
    if (!bind || !bind->ctx || !bind->ctx->analyzer || !name)
        return NULL;
    if (bind->exports) {
        XaSymbol *sym = (XaSymbol *) xr_hashmap_get(bind->exports, name);
        if (sym)
            return sym;
    }
    return xa_default_arg_find_decl_file_symbol(
        bind->ctx->analyzer, bind->ctx->analyzer->global_scope, bind->decl_file, name);
}

static void xa_bind_default_arg_export_symbols(AstNode *node, XaDefaultArgBindCtx *bind) {
    if (!node || !bind)
        return;
    if (node->type >= AST_BINARY_ADD && node->type <= AST_BINARY_OR) {
        xa_bind_default_arg_export_symbols(node->as.binary.left, bind);
        xa_bind_default_arg_export_symbols(node->as.binary.right, bind);
        return;
    }
    if (node->type >= AST_UNARY_NEG && node->type <= AST_UNARY_BNOT) {
        xa_bind_default_arg_export_symbols(node->as.unary.operand, bind);
        return;
    }
    switch (node->type) {
        case AST_VARIABLE:
            if (node->as.variable.name) {
                XaSymbol *sym = xa_default_arg_lookup_decl_symbol(bind, node->as.variable.name);
                if (sym) {
                    node->as.variable.symbol_id = sym->id;
                    break;
                }
                XaSymbol *current = node->as.variable.symbol_id
                                        ? xa_scope_lookup_by_id(bind->ctx->analyzer->global_scope,
                                                                node->as.variable.symbol_id)
                                        : NULL;
                if (!xa_default_arg_symbol_is_from_decl_file(
                        bind->ctx->analyzer, current, bind->decl_file, node->as.variable.name))
                    node->as.variable.symbol_id = 0;
            }
            break;
        case AST_MEMBER_ACCESS:
            xa_bind_default_arg_export_symbols(node->as.member_access.object, bind);
            break;
        case AST_CALL_EXPR:
            xa_bind_default_arg_export_symbols(node->as.call_expr.callee, bind);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                xa_bind_default_arg_export_symbols(node->as.call_expr.arguments[i], bind);
            break;
        case AST_GROUPING:
            xa_bind_default_arg_export_symbols(node->as.grouping, bind);
            break;
        case AST_TERNARY:
            xa_bind_default_arg_export_symbols(node->as.ternary.condition, bind);
            xa_bind_default_arg_export_symbols(node->as.ternary.true_expr, bind);
            xa_bind_default_arg_export_symbols(node->as.ternary.false_expr, bind);
            break;
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                xa_bind_default_arg_export_symbols(node->as.array_literal.elements[i], bind);
            xa_bind_default_arg_export_symbols(node->as.array_literal.repeat_value, bind);
            xa_bind_default_arg_export_symbols(node->as.array_literal.repeat_count, bind);
            break;
        case AST_TUPLE_LITERAL:
            for (int i = 0; i < node->as.tuple_literal.count; i++)
                xa_bind_default_arg_export_symbols(node->as.tuple_literal.elements[i], bind);
            break;
        case AST_SPREAD_EXPR:
            xa_bind_default_arg_export_symbols(node->as.spread_expr.expr, bind);
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                xa_bind_default_arg_export_symbols(node->as.object_literal.keys[i], bind);
                xa_bind_default_arg_export_symbols(node->as.object_literal.values[i], bind);
            }
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                xa_bind_default_arg_export_symbols(node->as.map_literal.keys[i], bind);
                xa_bind_default_arg_export_symbols(node->as.map_literal.values[i], bind);
            }
            break;
        case AST_SET_LITERAL:
            for (int i = 0; i < node->as.set_literal.count; i++)
                xa_bind_default_arg_export_symbols(node->as.set_literal.elements[i], bind);
            break;
        case AST_INDEX_GET:
            xa_bind_default_arg_export_symbols(node->as.index_get.array, bind);
            xa_bind_default_arg_export_symbols(node->as.index_get.index, bind);
            break;
        case AST_SLICE_EXPR:
            xa_bind_default_arg_export_symbols(node->as.slice_expr.source, bind);
            xa_bind_default_arg_export_symbols(node->as.slice_expr.start, bind);
            xa_bind_default_arg_export_symbols(node->as.slice_expr.end, bind);
            break;
        case AST_NEW_EXPR:
            for (int i = 0; i < node->as.new_expr.arg_count; i++)
                xa_bind_default_arg_export_symbols(node->as.new_expr.arguments[i], bind);
            break;
        case AST_STRUCT_LITERAL:
            for (int i = 0; i < node->as.struct_literal.field_count; i++)
                xa_bind_default_arg_export_symbols(node->as.struct_literal.field_values[i], bind);
            break;
        case AST_OPTIONAL_CHAIN:
            xa_bind_default_arg_export_symbols(node->as.optional_chain.object, bind);
            xa_bind_default_arg_export_symbols(node->as.optional_chain.index, bind);
            break;
        case AST_RANGE:
            xa_bind_default_arg_export_symbols(node->as.range.start, bind);
            xa_bind_default_arg_export_symbols(node->as.range.end, bind);
            break;
        case AST_IS_EXPR:
            xa_bind_default_arg_export_symbols(node->as.is_expr.expr, bind);
            break;
        case AST_AS_EXPR:
            xa_bind_default_arg_export_symbols(node->as.as_expr.expr, bind);
            break;
        default:
            break;
    }
}

static bool xa_complete_call_default_args(XaInferContext *ctx, CallExprNode *call,
                                          XaSymbolLinks *links, int param_count) {
    if (!ctx || !call || !links || !links->param_defaults || param_count <= 0 ||
        links->param_count != param_count || call->arg_count >= param_count)
        return false;

    bool can_complete = true;
    int supplied_arg_count = call->arg_count;
    int required_arg_count = 0;
    for (int i = 0; i < param_count; i++) {
        if (!links->param_defaults[i])
            required_arg_count++;
    }
    for (int i = 0; i < call->arg_count; i++) {
        if (call->arguments[i] && call->arguments[i]->type == AST_SPREAD_EXPR) {
            can_complete = false;
            break;
        }
    }
    for (int i = call->arg_count; can_complete && i < param_count; i++) {
        if (!links->param_defaults[i])
            can_complete = false;
    }
    if (!can_complete)
        return false;

    XrCompilerSession *sess =
        ctx->analyzer ? xr_compiler_session_current_for_isolate(ctx->analyzer->isolate) : NULL;
    AstNode **new_args = (AstNode **) xr_calloc((size_t) param_count, sizeof(AstNode *));
    if (!new_args)
        return false;
    XrCallArgAccess *new_accesses =
        (XrCallArgAccess *) xr_calloc((size_t) param_count, sizeof(XrCallArgAccess));
    if (!new_accesses) {
        xr_free(new_args);
        return false;
    }
    for (int i = 0; i < call->arg_count; i++) {
        new_args[i] = call->arguments[i];
        new_accesses[i] = call->arg_accesses ? call->arg_accesses[i] : XR_CALL_ARG_PLAIN;
    }
    XrHashMap *decl_exports = xa_default_arg_decl_exports(ctx, links);
    XaDefaultArgBindCtx bind = {
        .ctx = ctx,
        .exports = decl_exports,
        .decl_file = links->file_path,
    };
    for (int i = call->arg_count; i < param_count; i++) {
        new_args[i] = xr_ast_clone_session(links->param_defaults[i], sess);
        xa_bind_default_arg_export_symbols(new_args[i], &bind);
    }
    call->arguments = new_args;
    call->arg_accesses = new_accesses;
    call->arg_count = param_count;
    call->supplied_arg_count = supplied_arg_count;
    call->default_arg_count = param_count - supplied_arg_count;
    call->default_arg_param_count = param_count;
    call->required_arg_count = required_arg_count;
    return true;
}

static void xa_mark_call_default_arg_contract(CallExprNode *call, XaSymbolLinks *links,
                                              int param_count) {
    if (!call || !links || !links->param_defaults || links->param_count != param_count ||
        param_count <= 0)
        return;
    int required_arg_count = 0;
    bool has_default = false;
    for (int i = 0; i < param_count; i++) {
        if (links->param_defaults[i])
            has_default = true;
        else
            required_arg_count++;
    }
    if (!has_default)
        return;
    if (call->supplied_arg_count < 0 || call->supplied_arg_count > call->arg_count)
        call->supplied_arg_count = call->arg_count;
    call->default_arg_param_count = param_count;
    call->required_arg_count = required_arg_count;
    call->default_arg_count =
        param_count > call->supplied_arg_count ? param_count - call->supplied_arg_count : 0;
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
    XrClassInfo *class_info =
        XR_TYPE_IS_INSTANCE(receiver_type) ? receiver_type->instance.class_ref : NULL;
    XaSymbolLinks *class_links = NULL;
    if (class_name) {
        XaSymbol *class_sym = xa_lookup_visible_class_symbol(ctx, class_name);
        class_links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
        if (!class_info && class_links)
            class_info = class_links->class_info;
    }
    if (!class_info)
        return NULL;
    XaSymbol *method_sym = xa_class_info_lookup_member(class_info, method_name);
    XaSymbolLinks *method_links =
        method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
    if (method_links && !method_links->file_path && class_info)
        method_links->file_path = class_info->location.file;
    return method_links;
}

static bool xa_call_mutates_receiver(XaInferContext *ctx, XrType *receiver_type,
                                     const char *method_name) {
    if (!ctx || !receiver_type || !method_name)
        return false;

    if (XR_TYPE_IS_STRING(receiver_type))
        return false;

    /* The sealed builtin registry is the canonical effect contract for
     * language-provided receiver methods.  Builtin classes also have class
     * metadata, but those symbols do not necessarily carry the inferred
     * mutates_receiver bit; consult the registry before user-class metadata. */
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (spec->effect == XA_BUILTIN_EFFECT_MUTATES_RECEIVER &&
            xa_builtin_receiver_matches_type(receiver_type, spec->receiver) &&
            strcmp(spec->source_name, method_name) == 0)
            return true;
    }
    if (xa_builtin_member_mutates_receiver(receiver_type, method_name))
        return true;

    const char *class_name = xr_type_get_class_name(receiver_type);
    XaBuiltinMethodMemoryEffectSet named_effects = XA_BUILTIN_MEMORY_STABLE_READ;
    if (xa_builtin_named_receiver_memory_effect(class_name, method_name, &named_effects) &&
        (named_effects & XA_BUILTIN_MEMORY_WRITE) != 0)
        return true;
    XrClassInfo *class_info =
        XR_TYPE_IS_INSTANCE(receiver_type) ? receiver_type->instance.class_ref : NULL;
    if (class_name) {
        XaSymbol *class_sym = xa_lookup_visible_class_symbol(ctx, class_name);
        XaSymbolLinks *class_links =
            class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
        if (!class_info && class_links)
            class_info = class_links->class_info;
        if (class_info) {
            XaSymbol *method_sym = xa_class_info_lookup_instance_member(class_info, method_name);
            return method_sym && method_sym->kind == XA_SYM_METHOD && method_sym->mutates_receiver;
        }
    }

    return false;
}

static bool xa_class_name_matches_mono_base(const char *class_name, const char *base) {
    if (!class_name || !base)
        return false;
    size_t n = strlen(base);
    if (strncmp(class_name, base, n) != 0)
        return false;
    return class_name[n] == '\0' || class_name[n] == '$';
}

static XaSymbolLinks *xa_type_capability_class_links(XaInferContext *ctx, XrType *type) {
    if (!ctx || !type)
        return NULL;
    const char *class_name = xr_type_get_class_name(type);
    XaSymbol *class_sym = class_name ? xa_lookup_visible_class_symbol(ctx, class_name) : NULL;
    if (!class_sym && class_name) {
        const char *mono = strchr(class_name, '$');
        if (mono && mono != class_name) {
            char origin[160];
            size_t n = (size_t) (mono - class_name);
            if (n < sizeof(origin)) {
                memcpy(origin, class_name, n);
                origin[n] = '\0';
                class_sym = xa_lookup_visible_class_symbol(ctx, origin);
            }
        }
    }
    return class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
}

static bool xa_type_allows_capabilities(XaInferContext *ctx, XrType *type, uint32_t required) {
    if (xa_type_has_capabilities(type, required))
        return true;
    XaSymbolLinks *class_links = xa_type_capability_class_links(ctx, type);
    if (!class_links)
        return false;
    if ((class_links->type && xa_type_has_capabilities(class_links->type, required)) ||
        (class_links->class_info &&
         (class_links->class_info->capability_flags & required) == required))
        return true;
    const char *class_name = xr_type_get_class_name(type);
    char origin[160];
    const char *mono = class_name ? strchr(class_name, '$') : NULL;
    if (mono && mono != class_name && (size_t) (mono - class_name) < sizeof(origin)) {
        size_t n = (size_t) (mono - class_name);
        memcpy(origin, class_name, n);
        origin[n] = '\0';
        class_name = origin;
    }
    uint32_t declared = xa_stdlib_type_capability_flags(class_links->module_name, class_name);
    return (declared & required) == required;
}

static bool xa_type_allows_interior_mutation(XaInferContext *ctx, XrType *type) {
    if (type && type->semantic_type_id == XA_SEMANTIC_TYPE_PARALLEL_PLAN)
        return true;
    return xa_type_allows_capabilities(ctx, type, XA_TYPE_CAP_INTERIOR_MUTABLE) ||
           xa_type_is_sys_threadlocal(ctx, type);
}

static bool xa_type_allows_shared_interior_mutation(XaInferContext *ctx, XrType *type) {
    const uint32_t required = XA_TYPE_CAP_INTERIOR_MUTABLE | XA_TYPE_CAP_SYNC_SHAREABLE;
    return xa_type_allows_capabilities(ctx, type, required) ||
           xa_type_is_sys_threadlocal(ctx, type);
}

static void xa_check_borrowed_escaping_param_arg(XaInferContext *ctx, AstNode *call_node,
                                                 XaSymbolLinks *callee_links,
                                                 const char *callee_name, AstNode *arg_node,
                                                 XrType *arg_type, int slot) {
    const XaParamEffectSummary *effect = xa_symbol_param_effect(callee_links, slot);
    if (!ctx || !call_node || !xa_param_effect_retains_or_escapes(effect))
        return;
    /* A closure literal is normally call-bounded, so its captures loan nothing
     * past the statement. A callee that retains or escapes the parameter
     * breaks that: the closure -- and every root it captured by reference --
     * outlives the call, and function-local analysis cannot follow it. */
    if (arg_node && arg_node->type == AST_FUNCTION_EXPR)
        xa_escape_pending_captures(ctx);
    if (arg_type && XR_TYPE_IS_POINTER(arg_type)) {
        char context[192];
        snprintf(context, sizeof(context),
                 "pass raw pointer borrow to escaping parameter %d of '%s'", slot + 1,
                 callee_name ? callee_name : "callee");
        xa_check_pointer_borrow_escape(ctx, arg_node ? arg_node : call_node, arg_node, arg_type,
                                       context);
        return;
    }
    if (xa_type_contains_span_view(arg_type)) {
        XaEscapeDestinationSet nonlocal_escape =
            effect ? (XaEscapeDestinationSet) (effect->escapes & ~XA_ESCAPE_LOCAL_STORAGE)
                   : XA_ESCAPE_NONE;
        if (callee_links && callee_links->return_view.complete &&
            callee_links->return_view.origin == XR_VIEW_RETURN_PARAM &&
            callee_links->return_view.param_index == slot && effect &&
            effect->returns != XA_RETURN_PROVENANCE_NONE && nonlocal_escape == XA_ESCAPE_NONE)
            return;
        char context[160];
        snprintf(context, sizeof(context), "pass Slice view to escaping parameter %d of '%s'",
                 slot + 1, callee_name ? callee_name : "callee");
        xa_check_span_value_escape(ctx, arg_node ? arg_node : call_node, arg_type, context);
        return;
    }
    if (!xa_type_needs_borrow_escape_guard(arg_type))
        return;
    XaSymbol *borrowed_root = xa_borrowed_param_root_symbol(ctx, arg_node);
    if (!borrowed_root || borrowed_root->passing_mode != XR_PARAM_REF)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node && arg_node->line ? arg_node->line : call_node->line,
                      .column =
                          arg_node && arg_node->column ? arg_node->column : call_node->column};
    const char *name = borrowed_root->name ? borrowed_root->name : "?";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot pass borrowed '%s' parameter '%s' to escaping parameter %d of '%s'; pass an "
             "owned value or copy(%s)",
             "ref", name, slot + 1, callee_name ? callee_name : "callee", name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_shared_mutating_param_arg(XaInferContext *ctx, AstNode *call_node,
                                               XaSymbolLinks *callee_links, const char *callee_name,
                                               AstNode *arg_node, XrType *arg_type, int slot) {
    const XaParamEffectSummary *effect = xa_symbol_param_effect(callee_links, slot);
    if (!ctx || !call_node || !xa_param_effect_mutates(effect))
        return;
    if (!xa_type_needs_borrow_escape_guard(arg_type))
        return;
    if (xa_type_allows_shared_interior_mutation(ctx, arg_type))
        return;
    if (!xa_expr_yields_shared_provenance(ctx, arg_node ? arg_node : call_node, arg_type))
        return;

    XaSymbol *root = xa_call_root_variable_symbol(ctx, arg_node);
    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node && arg_node->line ? arg_node->line : call_node->line,
                      .column =
                          arg_node && arg_node->column ? arg_node->column : call_node->column};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot pass const-derived value '%s' to mutating parameter %d of '%s'; pass "
             "copy(%s) or use an audited synchronization handle",
             root && root->name ? root->name : "?", slot + 1, callee_name ? callee_name : "callee",
             root && root->name ? root->name : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static bool xa_call_is_unknown_function_value_callee(CallExprNode *call, XaSymbol *fn_sym,
                                                     XaSymbolLinks *fn_links, XrType *callee_type,
                                                     int slot) {
    if (!call || !call->callee || !callee_type || !XR_TYPE_IS_FUNCTION(callee_type) || slot < 0)
        return false;
    if (call->callee->type != AST_VARIABLE)
        return false;
    if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION)
        return false;
    if (fn_sym && fn_sym->kind == XA_SYM_CLASS)
        return false;
    const XaParamEffectSummary *effect = xa_symbol_param_effect(fn_links, slot);
    return !effect || !effect->complete;
}

static bool xa_call_has_unknown_function_value_escape(CallExprNode *call, XaSymbol *fn_sym,
                                                      XaSymbolLinks *fn_links, XrType *callee_type,
                                                      int slot) {
    if (!call || !call->callee || !callee_type || !XR_TYPE_IS_FUNCTION(callee_type) || slot < 0)
        return false;
    if (call->callee->type != AST_VARIABLE)
        return false;
    if (fn_sym && (fn_sym->kind == XA_SYM_FUNCTION || fn_sym->kind == XA_SYM_CLASS))
        return false;
    const XaParamEffectSummary *effect = xa_symbol_param_effect(fn_links, slot);
    return !effect || !effect->complete;
}

static void xa_check_unknown_function_value_memory_effect_actual(
    XaInferContext *ctx, AstNode *call_node, CallExprNode *call, XaSymbol *fn_sym,
    XaSymbolLinks *fn_links, XrType *callee_type, AstNode *actual, int slot,
    const char *callee_name) {
    if (!ctx || !call_node || !actual ||
        !xa_call_is_unknown_function_value_callee(call, fn_sym, fn_links, callee_type, slot) ||
        xa_call_param_mode(callee_type, slot) != XR_PARAM_REF)
        return;
    XaMemoryEffectSummary unknown;
    xa_memory_effect_summary_init(&unknown);
    xa_memory_effect_summary_mark_incomplete(&unknown, XA_UNKNOWN_VIEW_INVALIDATION);
    xa_check_call_memory_effect_actual(ctx, call_node, &unknown, XA_MEMORY_ROOT_PARAM,
                                       (uint32_t) slot, actual, callee_name);
    xa_memory_effect_summary_clear(&unknown);
}

static void xa_check_pointer_unknown_function_value_arg(XaInferContext *ctx, AstNode *call_node,
                                                        CallExprNode *call, XaSymbol *fn_sym,
                                                        XaSymbolLinks *fn_links,
                                                        XrType *callee_type, AstNode *arg_node,
                                                        XrType *arg_type, int slot) {
    if (!ctx || !call_node || !call || !arg_node || !arg_type || !XR_TYPE_IS_POINTER(arg_type) ||
        !xa_call_has_unknown_function_value_escape(call, fn_sym, fn_links, callee_type, slot))
        return;
    const char *callee_name = call->callee && call->callee->type == AST_VARIABLE
                                  ? call->callee->as.variable.name
                                  : "function value";
    char context[224];
    snprintf(context, sizeof(context),
             "pass raw pointer borrow to parameter %d of function value '%s' with unknown escape "
             "summary",
             slot + 1, callee_name ? callee_name : "?");
    xa_check_pointer_borrow_escape(ctx, arg_node, arg_node, arg_type, context);
}

static void xa_check_shared_unknown_function_value_arg(XaInferContext *ctx, AstNode *call_node,
                                                       CallExprNode *call, XaSymbol *fn_sym,
                                                       XaSymbolLinks *fn_links, XrType *callee_type,
                                                       AstNode *arg_node, XrType *arg_type,
                                                       int slot) {
    if (!ctx || !call_node || !call ||
        !xa_call_is_unknown_function_value_callee(call, fn_sym, fn_links, callee_type, slot))
        return;
    if (!xa_type_needs_borrow_escape_guard(arg_type))
        return;
    if (xa_type_allows_shared_interior_mutation(ctx, arg_type))
        return;
    if (!xa_expr_yields_shared_provenance(ctx, arg_node ? arg_node : call_node, arg_type))
        return;

    XaSymbol *root = xa_call_root_variable_symbol(ctx, arg_node);
    const char *callee_name = call->callee && call->callee->type == AST_VARIABLE
                                  ? call->callee->as.variable.name
                                  : "function value";
    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node && arg_node->line ? arg_node->line : call_node->line,
                      .column =
                          arg_node && arg_node->column ? arg_node->column : call_node->column};
    char msg[288];
    snprintf(msg, sizeof(msg),
             "cannot pass const-derived value '%s' to function value '%s' with unknown mutation "
             "summary; pass copy(%s) or call a known function directly",
             root && root->name ? root->name : "?", callee_name ? callee_name : "?",
             root && root->name ? root->name : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_transfer_storage_param_arg(XaInferContext *ctx, AstNode *call_node,
                                                XaSymbolLinks *callee_links,
                                                const char *callee_name, AstNode *arg_node,
                                                XrType *arg_type, int slot) {
    const XaParamEffectSummary *effect = xa_symbol_param_effect(callee_links, slot);
    if (!ctx || !call_node || !effect || effect->storage_domain != XR_STORAGE_TRANSFERABLE)
        return;
    if (!xa_boundary_transfer_type_needs_explicit(arg_type))
        return;
    XaSymbol *move_source = xa_boundary_move_source_symbol(ctx, arg_node);
    XaSymbolLinks *source_links =
        move_source ? xa_analyzer_get_links(ctx->analyzer, move_source) : NULL;
    if (source_links && source_links->root_id != 0 && source_links->root_alias == XA_ROOT_UNIQUE &&
        source_links->final_move.complete && source_links->allocation_plan.complete) {
        source_links->storage_domain = XR_STORAGE_TRANSFERABLE;
        source_links->allocation_plan.domain = XR_STORAGE_TRANSFERABLE;
        source_links->allocation_plan.materialization = XR_MATERIALIZE_SYSTEM_HEAP;
        source_links->allocation_plan.evidence |= XA_OWNERSHIP_EV_TRANSFER;
        return;
    }

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node && arg_node->line ? arg_node->line : call_node->line,
                      .column =
                          arg_node && arg_node->column ? arg_node->column : call_node->column};
    const char *source_name = move_source && move_source->name ? move_source->name : NULL;
    char msg[320];
    if (source_name) {
        snprintf(msg, sizeof(msg),
                 "argument %d for '%s' requires move of a proven unique transferable source; "
                 "end aliases/loans for '%s' first",
                 slot + 1, callee_name ? callee_name : "callee", source_name);
    } else {
        snprintf(msg, sizeof(msg),
                 "argument %d for '%s' requires move of a proven unique transferable source",
                 slot + 1, callee_name ? callee_name : "callee");
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static XrType *xa_math_runtime_shape_return_type(XaInferContext *ctx, CallExprNode *call,
                                                 XaSymbolLinks *fn_links, XrType **arg_types,
                                                 int arg_count) {
    const char *member = xa_math_call_member(ctx, call, fn_links);
    if (!member)
        return NULL;
    if (strcmp(member, "abs") == 0 && arg_count == 1 && xa_all_args_are_int(arg_types, 1))
        return xr_type_new_int(ctx && ctx->analyzer ? ctx->analyzer->isolate : NULL);
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
static bool xa_len_type_supported(XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_STRING:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_JSON:
        case XR_KIND_UNKNOWN:
            return true;
        case XR_KIND_TYPE_PARAM:
            /* Unconstrained synthetic parameters are used while higher-order
             * callback inference is still converging.  Defer their check to
             * the specialized call site; an explicit non-Lengthable
             * constraint is rejected immediately. */
            return !type->type_param.constraint ||
                   xr_type_is_builtin_named_type(type->type_param.constraint, "Lengthable");
        case XR_KIND_UNION:
            if (type->union_type.member_count == 0)
                return false;
            for (int i = 0; i < type->union_type.member_count; i++) {
                if (!xa_len_type_supported(type->union_type.members[i]))
                    return false;
            }
            return true;
        case XR_KIND_INSTANCE:
        case XR_KIND_CLASS: {
            const char *class_name = xr_type_get_class_name(type);
            XrClassInfo *info = type->instance.class_ref;
            if (info) {
                for (int i = 0; i < info->interface_count; i++) {
                    XrType *iface = info->interface_types ? info->interface_types[i] : NULL;
                    if (xr_type_is_builtin_named_type(iface, "Lengthable"))
                        return true;
                }
            }
            /* The name list below is the builtin allowlist: those types answer
             * len() natively without declaring Lengthable. A user class that
             * reuses one of the names has already had its own interfaces
             * checked above, so it must not inherit the builtin's answer —
             * `class Range { }` is not Lengthable and len() on it is a
             * compile error, not a runtime panic. */
            if (info)
                return false;
            return class_name &&
                   (strcmp(class_name, "StringBuilder") == 0 || strcmp(class_name, "Buffer") == 0 ||
                    strcmp(class_name, "WorkQueue") == 0 || strcmp(class_name, "Range") == 0);
        }
        default:
            return false;
    }
}

/* Generic higher-order calls are checked left-to-right.  By the time a callback argument is
 * visited, earlier value arguments may already determine some of the callee's type parameters
 * (`apply(5, fn(n) { ... })` determines T=int before the lambda).  Substitute that partial
 * solution into the current parameter so lambda parameters receive concrete contextual types;
 * unresolved type parameters deliberately remain unchanged. */
static XrType *xa_contextualize_generic_call_param(XaInferContext *ctx, XaSymbolLinks *links,
                                                   XrType *callee_type, CallExprNode *call,
                                                   XrType **effective_arg_types, int current_slot,
                                                   XrType *param_type) {
    if (!ctx || !ctx->analyzer || !links || !callee_type || !XR_TYPE_IS_FUNCTION(callee_type) ||
        !call || !param_type)
        return param_type;
    int count = xa_symbol_links_get_type_param_count(links);
    if (count <= 0)
        return param_type;

    const char *name_buf[8] = {0};
    XrType *actual_buf[8] = {0};
    const char **names = count <= 8 ? name_buf : xr_calloc((size_t) count, sizeof(const char *));
    XrType **actual = count <= 8 ? actual_buf : xr_calloc((size_t) count, sizeof(XrType *));
    if (!names || !actual) {
        if (names != name_buf)
            xr_free((void *) names);
        if (actual != actual_buf)
            xr_free(actual);
        return param_type;
    }

    for (int i = 0; i < count; i++)
        names[i] = xa_symbol_links_get_type_param_name(links, i);

    if (call->type_arg_count == count && call->type_args) {
        for (int i = 0; i < count; i++)
            actual[i] = call->type_args[i]
                            ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[i])
                            : NULL;
    } else if (call->type_arg_count == 0 && effective_arg_types) {
        int param_count = callee_type->function.param_count;
        int resolved_slots = current_slot < param_count ? current_slot : param_count;
        for (int i = 0; i < count; i++) {
            if (!names[i])
                continue;
            for (int slot = 0; slot < resolved_slots; slot++) {
                XrType *arg = effective_arg_types[slot];
                XrType *declared = xr_type_function_param_type(callee_type, slot);
                if (!arg || !declared)
                    continue;
                actual[i] = xa_infer_type_param_from_arg(declared, arg, names[i], 0);
                /* `null` satisfies T? but carries no evidence for T.  Keep scanning so a
                 * later concrete argument (valueOrDefault(null, 42)) determines it. */
                if (actual[i] && XR_TYPE_IS_NULL(actual[i]))
                    actual[i] = NULL;
                if (actual[i])
                    break;
            }
        }
    }

    /* xr_type_substitute treats a missing replacement as unknown.  Compact the partial
     * solution before substituting so class parameters already resolved on the receiver (for
     * example Plan<int>.forEach) are not erased merely because this method call supplies no new
     * evidence for them. */
    int resolved_count = 0;
    for (int i = 0; i < count; i++) {
        if (!names[i] || !actual[i] || XR_TYPE_IS_UNKNOWN(actual[i]) || XR_TYPE_IS_NULL(actual[i]))
            continue;
        names[resolved_count] = names[i];
        actual[resolved_count] = actual[i];
        resolved_count++;
    }
    XrType *result = resolved_count > 0 ? xr_type_substitute(ctx->analyzer->isolate, param_type,
                                                             names, actual, resolved_count)
                                        : param_type;
    if (names != name_buf)
        xr_free((void *) names);
    if (actual != actual_buf)
        xr_free(actual);
    return result ? result : param_type;
}

// Does one declared constraint on a type parameter already imply `required`?
// Reuses the runtime checker with the *constraint* as the candidate type, so
// identical interfaces, interface inheritance, class upper bounds and the
// universally-satisfied interfaces (Stringable / Equatable) all resolve there.
static bool xa_constraint_implies(XrType *have, XrType *required) {
    if (!required)
        return true;
    if (!have)
        return false;
    return xr_type_satisfies_constraint(have, required);
}

// Look for `name` in one declaration's type-parameter list. Sets `*out_found`
// when the declaration owns that parameter, so callers can stop the scope walk
// at the innermost binder instead of letting an outer `T` shadow back in.
static bool xa_type_param_link_implies(XaSymbolLinks *links, const char *name, XrType *required,
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
            if (xa_constraint_implies(constraints ? constraints[j] : NULL, required))
                return true;
        }
        return false;
    }
    return false;
}

// Constraint entailment for a type parameter forwarded as a type argument.
//
// A type parameter's constraints live on the enclosing declaration's
// XaSymbolLinks, not on the XrType, so xr_type_satisfies_constraint() only ever
// sees a bare XR_KIND_TYPE_PARAM and has nothing to match. Resolve it here:
// walk out to the function or class that introduced `name` and accept when one
// of its own constraints implies `required`.
static bool xa_type_param_entails_constraint(XaInferContext *ctx, const char *name,
                                             XrType *required) {
    if (!ctx || !ctx->analyzer || !name)
        return false;
    for (XaScope *scope = ctx->analyzer->current_scope; scope; scope = scope->parent) {
        bool found = false;
        if (scope->function_symbol) {
            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, scope->function_symbol);
            if (xa_type_param_link_implies(links, name, required, &found))
                return true;
            if (found)
                return false;
        }
        if (scope->class_symbol) {
            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, scope->class_symbol);
            if (xa_type_param_link_implies(links, name, required, &found))
                return true;
            if (found)
                return false;
        }
    }
    return false;
}

// Constraint check for a generic call's type argument.  Same contract as
// xr_type_satisfies_constraint(), plus the analyzer-only entailment path that
// lets a constrained type parameter be forwarded into another generic.
static bool xa_type_arg_satisfies_constraint(XaInferContext *ctx, XrType *type_arg,
                                             XrType *constraint) {
    if (xr_type_satisfies_constraint(type_arg, constraint))
        return true;
    if (type_arg && type_arg->kind == XR_KIND_TYPE_PARAM)
        return xa_type_param_entails_constraint(ctx, type_arg->type_param.name, constraint);
    return false;
}

// Name the callee for a diagnostic. Deliberately reports only the written
// name, never "function" or "method": one call site here may be a plain
// function, a static method, an instance method, or a module member, and the
// callee syntax alone cannot tell them apart.
static const char *xa_callee_display_name(CallExprNode *call) {
    const char *name = NULL;
    if (call && call->callee) {
        if (call->callee->type == AST_VARIABLE)
            name = call->callee->as.variable.name;
        else if (call->callee->type == AST_MEMBER_ACCESS)
            name = call->callee->as.member_access.name;
    }
    return name ? name : "<anonymous>";
}

// Verify explicit type arguments — both their count and each one against the
// declaration's `<T: A & B>` intersection constraint list. Shared by
// plain-function / static-method calls and by instance `obj.method<T>()`
// calls, which resolve their links later. Arity and constraints stay in one
// helper so a caller can never pick up half the checking.
static void xa_check_explicit_type_args(XaInferContext *ctx, AstNode *node, CallExprNode *call,
                                        XaSymbolLinks *links) {
    if (!ctx || !node || !call || !links || call->type_arg_count <= 0 || !call->type_args)
        return;

    int expected_count = xa_symbol_links_get_type_param_count(links);

    if (call->type_arg_count != expected_count) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "Generic call to '%s' expects %d type argument(s), but got %d",
                 xa_callee_display_name(call), expected_count, call->type_arg_count);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT,
                                   msg, &loc);
    }

    for (int i = 0; i < call->type_arg_count && i < expected_count; i++) {
        // Use analyzer-aware resolver so user class type-args carry their
        // superclass chain — required for `<T: BaseClass>` upper bounds.
        XrType *type_arg = call->type_args[i]
                               ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[i])
                               : NULL;
        if (xa_reject_error_type_success_type(ctx->analyzer, type_arg, "generic type argument",
                                              "function", node->line, node->column)) {
            continue;
        }

        int constraint_count = 0;
        XrType **constraints =
            xa_symbol_links_get_type_param_constraints(links, i, &constraint_count);

        if (!type_arg || constraint_count == 0)
            continue;

        for (int j = 0; j < constraint_count; j++) {
            XrType *constraint = constraints[j];
            if (constraint && !xa_type_arg_satisfies_constraint(ctx, type_arg, constraint)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                const char *param_name = xa_symbol_links_get_type_param_name(links, i);
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
}

// Implicit generic instantiation: no explicit `<T>` at the call site, so infer
// each type parameter from the first argument whose declared type is exactly
// the bare T, then verify that parameter's constraints. Shared by plain-function
// calls and by instance `obj.method(...)` calls, which resolve their links later.
static void xa_check_inferred_type_arg_constraints(XaInferContext *ctx, AstNode *node,
                                                   CallExprNode *call, XaSymbolLinks *links) {
    if (!ctx || !node || !call || !links)
        return;

    int tp_count = xa_symbol_links_get_type_param_count(links);
    int p_count = links->param_count;
    for (int ti = 0; ti < tp_count; ti++) {
        int constraint_count = 0;
        XrType **constraints =
            xa_symbol_links_get_type_param_constraints(links, ti, &constraint_count);
        if (constraint_count == 0)
            continue;
        const char *tp_name = xa_symbol_links_get_type_param_name(links, ti);
        if (!tp_name)
            continue;
        // Find the first parameter whose type is exactly the bare T
        XrType *inferred = NULL;
        for (int pi = 0; pi < p_count && pi < call->arg_count; pi++) {
            XrType *pt = links->param_types ? links->param_types[pi] : NULL;
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
            if (constraint && !xa_type_arg_satisfies_constraint(ctx, inferred, constraint)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Type '%s' does not satisfy constraint '%s' for type parameter '%s'",
                         xr_type_to_string(inferred), xr_type_to_string(constraint), tp_name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
            }
        }
    }
}

XrType *xa_visit_call(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_error(NULL);

    CallExprNode *call = &node->as.call_expr;
    /* `f?.(args)` (chain_type 3) and `obj?.method(args)` (chain_type 2) both
     * short-circuit to null when the receiver is null, so the call's result is
     * nullable in both shapes (spec §3.6). */
    bool optional_function_call = call->callee && call->callee->type == AST_OPTIONAL_CHAIN &&
                                  (call->callee->as.optional_chain.chain_type == 3 ||
                                   call->callee->as.optional_chain.chain_type == 2);

    if (call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
        strcmp(call->callee->as.variable.name, "len") == 0 && call->arg_count == 1) {
        if (xa_call_has_explicit_arg_access(call)) {
            xa_report_arg_accesses_require_known_contract(ctx, node, call);
            return xr_type_new_int(ctx->analyzer->isolate);
        }
        XrType *saved_expected = ctx->expected_type;
        bool saved_view_context = ctx->allow_view_expr_for_copy;
        if (xa_expr_needs_contextual_view_type(call->arguments[0])) {
            AstNode *operand = call->arguments[0];
            while (operand && operand->type == AST_GROUPING)
                operand = operand->as.grouping;
            bool is_string_bytes =
                operand && operand->type == AST_CALL_EXPR && operand->as.call_expr.callee &&
                operand->as.call_expr.callee->type == AST_MEMBER_ACCESS &&
                operand->as.call_expr.callee->as.member_access.name &&
                strcmp(operand->as.call_expr.callee->as.member_access.name, "bytes") == 0;
            ctx->expected_type =
                is_string_bytes ? xr_type_new_u8_slice(ctx->analyzer->isolate)
                                : xr_type_new_slice(ctx->analyzer->isolate,
                                                    xr_type_new_unknown(ctx->analyzer->isolate));
            ctx->allow_view_expr_for_copy = true;
        }
        XrType *operand_type = xa_visit_infer_expr(ctx, call->arguments[0]);
        ctx->expected_type = saved_expected;
        ctx->allow_view_expr_for_copy = saved_view_context;
        /* len() dereferences its argument, so a still-nullable one is the same
         * error as `x.f` on a nullable receiver (spec §2.13 N-12). Checked
         * before the Lengthable test, which sees through the nullable flag and
         * would otherwise accept `Array<int>?` and defer the failure to a
         * runtime panic. */
        if (!xa_check_nullable_access(ctx, node, call->arguments[0], operand_type, "len()",
                                      false) &&
            !xa_len_type_supported(operand_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "len() requires a Lengthable value, got '%s'; iterators and streams must "
                     "use count()",
                     operand_type ? xr_type_to_string(operand_type) : "unknown");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
        return xr_type_new_int(ctx->analyzer->isolate);
    }

    bool runtime_intrinsic_handled = false;
    XrType *runtime_intrinsic_type =
        xa_visit_coro_local_constructor(ctx, node, call, &runtime_intrinsic_handled);
    if (runtime_intrinsic_handled)
        return runtime_intrinsic_type;
    runtime_intrinsic_type = xa_visit_coro_pool_submit(ctx, node, call, &runtime_intrinsic_handled);
    if (runtime_intrinsic_handled)
        return runtime_intrinsic_type;

    if (xa_call_is_sys_thread_spawn(call))
        return xa_visit_sys_thread_spawn_call(ctx, node, call);

    // Record dependency: current function depends on called function
    XaSymbol *fn_sym = NULL;
    XaSymbolLinks *fn_links = NULL;
    XaSymbolLinks *class_ctor_links = NULL;

    if (call->callee && call->callee->type == AST_VARIABLE) {
        const char *fn_name = call->callee->as.variable.name;
        if (xa_freestanding_profile_enabled(ctx->analyzer) &&
            xa_freestanding_builtin_call_rejected(fn_name)) {
            char feature[160];
            snprintf(feature, sizeof(feature), "builtin %s()", fn_name ? fn_name : "?");
            xa_freestanding_report_unavailable(
                ctx, node, feature,
                "this builtin depends on hosted conversion, cloning, or debug helpers");
        }
        fn_sym = xa_lookup_visible_symbol(ctx, fn_name);
        if (!fn_sym && call->semantic_type_id != 0 && call->callee->as.variable.symbol_id != 0) {
            fn_sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope,
                                           call->callee->as.variable.symbol_id);
        }
        if (fn_sym) {
            fn_links = xa_refresh_imported_symbol_metadata(ctx, fn_sym);
            if (fn_sym->kind == XA_SYM_CLASS)
                class_ctor_links = xa_class_constructor_links(ctx, fn_sym);
            if (fn_sym->is_imported && fn_links && fn_links->module_name &&
                !xa_freestanding_stdlib_member_allowed(
                    fn_links->module_name,
                    fn_links->import_member_name ? fn_links->import_member_name : fn_sym->name)) {
                const char *member =
                    fn_links->import_member_name ? fn_links->import_member_name : fn_sym->name;
                char feature[192];
                snprintf(feature, sizeof(feature), "%s.%s", fn_links->module_name, member);
                xa_freestanding_report_unavailable(
                    ctx, node, feature,
                    xa_freestanding_stdlib_member_reject_suggestion(fn_links->module_name));
            }
        }
        if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION) {
            // FFI: calling an extern function is unsafe — it crosses into a
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
        // for a bare-name call. Static methods use the same caller-side default
        // model, including module-exported classes. Instance methods are
        // resolved after the receiver object has been inferred.
        fn_links = xa_module_member_fn_links(ctx, call->callee);
        if (!fn_links)
            fn_links = xa_static_method_fn_links(ctx, call->callee);
        if (!fn_links) {
            XaSymbol *class_sym = xa_module_member_class_symbol(ctx, call->callee);
            class_ctor_links = xa_class_constructor_links(ctx, class_sym);
        }
    }

    const char *mem_layout_member = xa_mem_layout_call_member(ctx, call);
    if (mem_layout_member)
        return xa_mem_layout_return_type(ctx, node, call, mem_layout_member);
    const char *mem_pointer_member = xa_mem_pointer_constructor_member(ctx, call);
    if (mem_pointer_member)
        return xa_mem_pointer_constructor_return_type(ctx, node, call, mem_pointer_member);
    if (xa_mem_slice_call(ctx, call))
        return xa_mem_slice_return_type(ctx, node, call);
    if (xa_mem_assume_initialized_call(ctx, call))
        return xa_mem_assume_initialized_return_type(ctx, node, call);
    if (xa_mem_with_slice_mut_call(ctx, call))
        return xa_mem_with_slice_mut_return_type(ctx, node, call);
    const char *mem_access = xa_mem_access_member(ctx, call);
    if (mem_access)
        return xa_mem_access_return_type(ctx, node, call, mem_access);

    if (call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
        strcmp(call->callee->as.variable.name, "typeName") == 0 && call->type_arg_count > 0) {
        if (call->type_arg_count != 1) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Generic function 'typeName' expects 1 type argument(s), but got %d",
                     call->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
        }
        if (call->arg_count != 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[128];
            snprintf(msg, sizeof(msg), "Expected 0 argument(s), but got %d", call->arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_WRONG_ARG_COUNT, msg, &loc);
        }
        if (call->type_arg_count == 1 && call->type_args && call->type_args[0]) {
            XrType *target = xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0]);
            (void) xa_reject_error_type_success_type(ctx->analyzer, target, "generic type argument",
                                                     "typeName<T>()", node ? node->line : 0,
                                                     node ? node->column : 0);
        }
        return xr_type_new_string(ctx->analyzer->isolate);
    }

    // Check generic type arguments. Instance-method links are not resolved yet
    // at this point; those calls are checked further down, once the receiver
    // has been inferred.
    bool type_args_checked = false;
    if (call->type_arg_count > 0 && fn_links) {
        xa_check_explicit_type_args(ctx, node, call, fn_links);
        type_args_checked = true;
    } else if (fn_links && xa_symbol_links_get_type_param_count(fn_links) > 0 && call->callee &&
               call->callee->type == AST_VARIABLE) {
        // Implicit generic instantiation: type args inferred from arguments.
        // For each parameter typed as a bare T, infer T = type(arg) and
        // verify constraints on T.  This mirrors the explicit branch above
        // but does its own simple inference per type parameter.
        XaSymbol *fn_sym = xa_lookup_visible_symbol(ctx, call->callee->as.variable.name);
        if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION) {
            xa_check_inferred_type_arg_constraints(ctx, node, call,
                                                   xa_analyzer_get_links(ctx->analyzer, fn_sym));
            type_args_checked = true;
        }
    }

    // Recognize Json.decode<T>(data): compiler-generated typed decode
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS && call->type_arg_count == 1) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        if (ma->name && strcmp(ma->name, "decode") == 0 && ma->object &&
            ma->object->type == AST_VARIABLE && strcmp(ma->object->as.variable.name, "Json") == 0) {
            XrType *target_type =
                call->type_args[0] ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[0])
                                   : NULL;
            if (xa_reject_error_type_success_type(ctx->analyzer, target_type,
                                                  "generic type argument", "Json.decode<T>()",
                                                  node ? node->line : 0, node ? node->column : 0))
                return xr_type_new_error(NULL);

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
                return xr_type_new_error(ctx->analyzer->isolate);
            }

            for (int i = 0; i < target_type->object.field_count; i++) {
                XrType *field_type =
                    target_type->object.field_types ? target_type->object.field_types[i] : NULL;
                if (xr_type_is_json_decode_field_supported(field_type))
                    continue;
                const char *field_name =
                    target_type->object.field_names ? target_type->object.field_names[i] : "?";
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Json.decode<T>() field '%s' has unsupported type '%s'; supported field "
                         "types are null, bool, int, float, string, Json, nested Record, "
                         "Array<Json>, and "
                         "nullable variants",
                         field_name ? field_name : "?",
                         field_type ? xr_type_to_string(field_type) : "unknown");
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
                return xr_type_new_unknown(NULL);
            }

            // Validate: exactly 1 argument
            if (call->arg_count != 1) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE,
                                           "Json.decode<T>() expects exactly 1 argument", &loc);
                return xr_type_new_error(ctx->analyzer->isolate);
            }

            // Visit argument to ensure it's analyzed
            if (xa_call_has_explicit_arg_access(call)) {
                xa_report_arg_accesses_require_known_contract(ctx, node, call);
                return xr_type_new_unknown(NULL);
            }
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
        XrType *raw_pointer_namespace_type = xa_call_raw_pointer_type_namespace(ctx, ma->object);
        callee_obj_type = raw_pointer_namespace_type;
        if (!callee_obj_type)
            callee_obj_type = xa_visit_infer_expr(ctx, ma->object);
        if (!fn_links)
            fn_links = xa_static_method_fn_links_from_type(ctx, callee_obj_type, method_name);
        if (!fn_links)
            fn_links = xa_method_symbol_links_for_call(ctx, callee_obj_type, method_name);
        // An instance method's links only become available here, after the
        // receiver has been inferred — run the type-argument checks the block
        // above could not reach, for both `obj.method<T>()` and the inferred
        // `obj.method(arg)` form.
        if (!type_args_checked && fn_links) {
            if (call->type_arg_count > 0)
                xa_check_explicit_type_args(ctx, node, call, fn_links);
            else
                xa_check_inferred_type_arg_constraints(ctx, node, call, fn_links);
            type_args_checked = true;
        }
        xa_check_threadlocal_suspend_context(ctx, node, callee_obj_type, method_name);

        /* R2-2 stopgap: checked/saturating/overflows methods on fixed-width
         * int receivers would silently evaluate at i64 boundaries. */
        {
            char ofw_msg[320];
            if (xa_builtin_int_overflow_method_unsupported(callee_obj_type, method_name, ofw_msg,
                                                           sizeof(ofw_msg))) {
                XrLocation ofw_loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, ofw_msg, &ofw_loc);
                return xr_type_new_error(ctx->analyzer->isolate);
            }
        }

        if (xa_method_call_creates_span_borrow(ctx, callee_obj_type, method_name, fn_links) &&
            !ctx->allow_view_expr_for_copy) {
            xa_check_span_borrow_source_stable(ctx, call->callee, ma->object, method_name);
        }

        if (method_name && xa_call_mutates_receiver(ctx, callee_obj_type, method_name) &&
            xa_type_can_own_span_view(callee_obj_type)) {
            char owner_path[512] = {0};
            XaSymbol *owner = xa_span_borrow_owner_path_for_owner_expr(ctx, ma->object, owner_path,
                                                                       sizeof(owner_path));
            if (!owner)
                owner = xa_span_borrow_owner_receiver_symbol(ctx, ma->object, callee_obj_type);
            if (owner)
                xa_check_active_loan_owner_path_mutation(
                    ctx, call->callee, owner, owner_path[0] ? owner_path : NULL, method_name);
        }

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

        XaSymbol *read_param = xa_read_param_symbol_for_expr(ctx, ma->object);
        if (read_param && method_name) {
            bool call_mutates_receiver =
                xa_call_mutates_receiver(ctx, callee_obj_type, method_name);
            bool interior_mutation = xa_type_allows_interior_mutation(ctx, callee_obj_type);
            if (call_mutates_receiver && !interior_mutation) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "Cannot call mutating method '%s' on read parameter '%s' (readonly "
                         "capability)",
                         method_name, read_param->name ? read_param->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
        }
        if (method_name && callee_obj_type && !XR_TYPE_IS_SLICE(callee_obj_type) &&
            xa_call_mutates_receiver(ctx, callee_obj_type, method_name)) {
            XaSymbol *root = xa_root_variable_symbol_for_expr(ctx, ma->object);
            if (root)
                root->links.value_mutated = true;
            bool readonly_receiver = xr_type_is_const(callee_obj_type);
            bool interior_mutation = xa_type_allows_interior_mutation(ctx, callee_obj_type) ||
                                     (root && root->links.storage_domain == XR_STORAGE_SYNC_SHARED);
            bool shared_receiver = xa_symbol_has_shared_provenance(root);
            bool shared_interior_mutation =
                shared_receiver && (xa_type_allows_shared_interior_mutation(ctx, callee_obj_type) ||
                                    xa_type_allows_shared_interior_mutation(ctx, root->links.type));
            if (!interior_mutation && ((root && (root->is_readonly_binding ||
                                                 (shared_receiver && !shared_interior_mutation))) ||
                                       readonly_receiver)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[192];
                const char *label =
                    root && root->is_readonly_binding ? "const binding" : "readonly value";
                snprintf(msg, sizeof(msg), "Cannot call mutating method '%s' on %s '%s'",
                         method_name, label, root && root->name ? root->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
        }
        if (method_name && callee_obj_type && XR_TYPE_IS_SLICE(callee_obj_type) &&
            xa_call_mutates_receiver(ctx, callee_obj_type, method_name)) {
            XaSymbol *root = xa_root_variable_symbol_for_expr(ctx, ma->object);
            if (root && (root->is_const || root->is_readonly_binding ||
                         xa_symbol_has_shared_provenance(root))) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[192];
                const char *label =
                    root->is_const || root->is_readonly_binding ? "const view" : "readonly view";
                snprintf(msg, sizeof(msg), "Cannot call mutating method '%s' on %s '%s'",
                         method_name, label, root->name ? root->name : "?");
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

    XaIntrinsicId parallel_call_intrinsic =
        xa_parallel_module_call_intrinsic(ctx, call->callee, fn_links);
    XaIntrinsicId parallel_plan_intrinsic =
        xa_parallel_plan_call_intrinsic(ctx, callee_obj_type, method_name);
    xa_record_parallel_call_plan(ctx, node,
                                 parallel_call_intrinsic != XA_INTRINSIC_NONE
                                     ? parallel_call_intrinsic
                                     : parallel_plan_intrinsic);
    const XaParallelCallPlan *recorded_parallel_plan =
        xa_analyzer_get_parallel_call_plan(ctx->analyzer, node);
    const char *parallel_call_member =
        recorded_parallel_plan && !recorded_parallel_plan->is_plan_method
            ? xa_parallel_call_kind_name(recorded_parallel_plan->kind)
            : NULL;
    xa_check_parallel_options_workers_const(ctx, node, call, parallel_call_member);
    const XaParallelCallPlan *parallel_call_plan =
        xa_analyzer_get_parallel_call_plan(ctx->analyzer, node);

    const char *payload_enum_name = NULL;
    const char *payload_variant_name = NULL;
    const XaEnumVariantInfo *payload_variant =
        xa_call_payload_enum_variant(ctx, call, &payload_enum_name, &payload_variant_name);

    bool saved_payload_ctor_value = ctx->allow_payload_enum_ctor_value;
    if (payload_variant)
        ctx->allow_payload_enum_ctor_value = true;
    XrType *callee_type = xa_visit_infer_expr(ctx, call->callee);
    ctx->allow_payload_enum_ctor_value = saved_payload_ctor_value;
    if (optional_function_call && callee_type)
        callee_type = xr_type_non_nullable(ctx->analyzer->isolate, callee_type);
    /* Member lookup yields the declaration signature before the receiver's class arguments are
     * applied. Specialize the whole callable before contextual lambda inference; specializing
     * only the return below leaves parameters such as Plan<S>.forEach's callback as `(S, int)`
     * even when the receiver is Plan<int>. */
    if (callee_type && XR_TYPE_IS_FUNCTION(callee_type) && callee_obj_type &&
        XR_TYPE_IS_INSTANCE(callee_obj_type) && callee_obj_type->instance.class_name &&
        callee_obj_type->instance.type_arg_count > 0 && callee_obj_type->instance.type_args) {
        XaSymbol *class_sym = fn_sym && fn_sym->parent ? fn_sym->parent : NULL;
        if (!class_sym)
            class_sym =
                callee_obj_type->instance.class_ref && callee_obj_type->instance.class_ref->scope
                    ? callee_obj_type->instance.class_ref->scope->class_symbol
                    : NULL;
        if (!class_sym)
            class_sym =
                xa_scope_lookup(ctx->analyzer->current_scope, callee_obj_type->instance.class_name);
        XaSymbolLinks *class_links =
            class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
        int class_param_count = class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
        if (class_param_count == callee_obj_type->instance.type_arg_count) {
            const char *name_buf[8] = {0};
            const char **names = class_param_count <= 8
                                     ? name_buf
                                     : xr_malloc(sizeof(const char *) * class_param_count);
            if (names) {
                for (int i = 0; i < class_param_count; i++)
                    names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                XrType *specialized = xr_type_substitute(ctx->analyzer->isolate, callee_type, names,
                                                         callee_obj_type->instance.type_args,
                                                         callee_obj_type->instance.type_arg_count);
                if (specialized)
                    callee_type = specialized;
                if (names != name_buf)
                    xr_free((void *) names);
            }
        }
    }
    if (parallel_call_plan && (!callee_type || !XR_TYPE_IS_FUNCTION(callee_type))) {
        XrType *parallel_callable =
            xa_parallel_callable_type(ctx, call, callee_obj_type, parallel_call_plan);
        if (parallel_callable)
            callee_type = parallel_callable;
    }

    if (class_ctor_links && class_ctor_links->param_defaults) {
        if (call->arg_count < class_ctor_links->param_count) {
            xa_complete_call_default_args(ctx, call, class_ctor_links,
                                          class_ctor_links->param_count);
        } else if (call->arg_count == class_ctor_links->param_count) {
            xa_mark_call_default_arg_contract(call, class_ctor_links,
                                              class_ctor_links->param_count);
        }
    }

    const XaResolvedCall *resolved_intrinsic = xa_record_resolved_intrinsic_call(
        ctx, node, call->callee, callee_obj_type, fn_sym, fn_links);
    const XaIntrinsicDesc *intrinsic_desc =
        resolved_intrinsic ? xa_intrinsic_by_id(resolved_intrinsic->intrinsic_id) : NULL;

    /* Resolve symbol_ids in arguments before any early-return path.  Values whose type is
     * supplied by the parameter contract must wait for the detailed loop below; eagerly
     * visiting an empty container (like a lambda) loses that context and emits a spurious
     * inference diagnostic even though the call signature is authoritative. */
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *arg = call->arguments[i];
        if (!arg || xa_expr_needs_parameter_context(arg))
            continue;
        XrCallArgAccess access = xa_call_arg_access(call, i);
        if (access != XR_CALL_ARG_PLAIN &&
            !xa_call_slot_has_function_param_contract(callee_type, i) &&
            xa_call_direct_variable_type_without_read(ctx, arg))
            continue;
        if (call->arguments[i])
            xa_visit_infer_expr(ctx, call->arguments[i]);
    }
    xa_check_codegen_intrinsic_call(ctx, node, call, intrinsic_desc);
    xa_check_intrinsic_shuffle_lanes(ctx, node, call, intrinsic_desc);
    if (intrinsic_desc && intrinsic_desc->id == XA_INTRINSIC_CODEGEN_OPAQUE &&
        call->arg_count == 1 && call->arguments[0]) {
        /* The compiler intrinsic is definitionally type-preserving.  Resolve it
         * before generic-call recovery paths so a rebuilt imported stdlib graph
         * cannot turn Ptr<T> into an error merely because its source-level T
         * declaration is no longer the same type-parameter object. */
        XrType *opaque_type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[0]);
        return opaque_type ? opaque_type : xr_type_new_error(ctx->analyzer->isolate);
    }

    if (payload_variant) {
        xa_check_payload_enum_variant_call(ctx, node, call, payload_variant, payload_enum_name,
                                           payload_variant_name);
        return callee_type ? callee_type : xr_type_new_unknown(NULL);
    }

    /* Namespace-imported class construction (`ns.Class(...)` / `ns.Class<T>(...)`)
     * resolves to the class instance type, exactly like the bare `Class(...)`
     * path. Done before the unknown-callee/class-callee branches below, which
     * only recognise AST_VARIABLE callees, so the receiver keeps its class
     * identity for cross-module method resolution and AOT codegen. */
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        XrType *ns_instance = xa_module_member_class_instance_type(ctx, call);
        if (ns_instance) {
            xa_report_arg_accesses_require_known_contract(ctx, node, call);
            xa_check_threadlocal_initializer(ctx, node, call, ns_instance);
            xa_freestanding_report_unavailable(
                ctx, node, "class construction",
                "use structs or explicit raw-memory APIs in this profile");
            if (xa_freestanding_profile_enabled(ctx->analyzer))
                return xr_type_new_error(ctx->analyzer->isolate);
            return ns_instance;
        }
    }
    XrType *imported_semantic_instance = xa_imported_semantic_class_instance_type(ctx, node, call);
    if (imported_semantic_instance) {
        xa_report_arg_accesses_require_known_contract(ctx, node, call);
        xa_check_threadlocal_initializer(ctx, node, call, imported_semantic_instance);
        xa_freestanding_report_unavailable(
            ctx, node, "class construction",
            "use structs or explicit raw-memory APIs in this profile");
        if (xa_freestanding_profile_enabled(ctx->analyzer))
            return xr_type_new_error(ctx->analyzer->isolate);
        return imported_semantic_instance;
    }

    // A diagnosed callee failure is recovery poison, not an unresolved callable.
    // Arguments were still visited above so their independent diagnostics survive. A
    // canonical namespace class is checked first because source-only analysis can know its
    // semantic identity even when no module export graph is attached to the session.
    if (XR_TYPE_IS_ERROR(callee_type)) {
        // ref/move authorization is independent of callee type recovery: an
        // unresolved callee cannot provide the parameter contract required by
        // either access marker.
        xa_report_arg_accesses_require_known_contract(ctx, node, call);
        return callee_type;
    }

    // Unknown callee type preserves error recovery after imprecise analysis.
    if (XR_TYPE_IS_UNKNOWN(callee_type)) {
        // Check if callee is a class name - if so, return instance type
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *name = call->callee->as.variable.name;

            /* Atomic(expr): infer Atomic<T> from argument type */
            if (strcmp(name, "Atomic") == 0) {
                XrType *et = NULL;
                if (xa_call_has_explicit_arg_access(call)) {
                    xa_report_arg_accesses_require_known_contract(ctx, node, call);
                } else if (call->arg_count > 0 && call->arguments[0]) {
                    et = xa_visit_infer_expr(ctx, call->arguments[0]);
                }
                if (!et)
                    et = xr_type_new_unknown(NULL);
                XrType *args[1] = {et};
                return xr_type_new_generic_instance(ctx->analyzer->isolate, "Atomic", NULL, args,
                                                    1);
            }
            XaSymbol *visible_class = xa_lookup_visible_symbol(ctx, name);
            if (xa_symbol_is_sync_runtime_class(ctx, visible_class, name)) {
                xa_report_arg_accesses_require_known_contract(ctx, node, call);
                return xa_sync_runtime_construct_type(ctx, name, call);
            }
            if (strcmp(name, "Thread") == 0) {
                xa_report_arg_accesses_require_known_contract(ctx, node, call);
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                    "Thread handles can only be created by sys.Thread.spawn", &loc);
                return xr_type_new_error(ctx->analyzer->isolate);
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
                    xa_freestanding_report_unavailable(
                        ctx, node, "class construction",
                        "use structs or explicit raw-memory APIs in this profile");
                    if (xa_freestanding_profile_enabled(ctx->analyzer))
                        return xr_type_new_error(ctx->analyzer->isolate);
                    return xa_class_constructor_instance_type(ctx, node, call, name, links,
                                                              links->class_info);
                }
                if (xa_symbol_is_sync_runtime_class(ctx, sym, name)) {
                    xa_report_arg_accesses_require_known_contract(ctx, node, call);
                    return xa_sync_runtime_construct_type(ctx, name, call);
                }
            }

            // Built-in primitive class Exception (and bare construction of it):
            // `Exception(msg)` constructs the runtime exception instance.
            if (strcmp(name, "PanicInfo") == 0) {
                xa_report_arg_accesses_require_known_contract(ctx, node, call);
                return xr_type_new_named_instance(ctx->analyzer->isolate, "PanicInfo");
            }
        }
        xa_report_arg_accesses_require_known_contract(ctx, node, call);
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
                AstNode *arg = call->arguments[i];
                if (!arg)
                    continue;
                XrCallArgAccess access = xa_call_arg_access(call, i);
                if (access != XR_CALL_ARG_PLAIN &&
                    xa_call_direct_variable_type_without_read(ctx, arg))
                    continue;
                xa_visit_infer_expr(ctx, arg);
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
        xa_check_threadlocal_initializer(ctx, node, call, callee_type);

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
                return xr_type_new_error(ctx->analyzer->isolate);
            }
            XaSymbol *class_sym = xa_lookup_visible_symbol(ctx, class_name);
            if (!class_sym || class_sym->kind != XA_SYM_CLASS)
                class_sym = xa_scope_lookup(ctx->analyzer->global_scope, class_name);
            if ((!class_sym || class_sym->kind != XA_SYM_CLASS) && call->semantic_type_id != 0 &&
                fn_sym && fn_sym->kind == XA_SYM_CLASS) {
                class_sym = fn_sym;
            }
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (links && links->class_info) {
                    xa_freestanding_report_unavailable(
                        ctx, node, "class construction",
                        "use structs or explicit raw-memory APIs in this profile");
                    if (xa_freestanding_profile_enabled(ctx->analyzer))
                        return xr_type_new_error(ctx->analyzer->isolate);
                    return xa_class_constructor_instance_type(ctx, node, call, class_name, links,
                                                              links->class_info);
                }
                if (xa_symbol_is_sync_runtime_class(ctx, class_sym, class_name))
                    return xa_sync_runtime_construct_type(ctx, class_name, call);
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
            xa_report_arg_accesses_require_known_contract(ctx, node, call);
            return callee_type;
        }
        /* A nullable function value is a null-safety problem, not a
         * "not callable" problem: report the cause and the fix (spec §2.13
         * N-12). */
        if (!xa_check_nullable_access(ctx, node, call->callee, callee_type, "call", true)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_NOT_CALLABLE, "Value is not callable", &loc);
        }
        xa_report_arg_accesses_require_known_contract(ctx, node, call);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    // Infer argument types
    int param_count = callee_type->function.param_count;
    bool is_variadic = callee_type->function.is_variadic;
    int rest_param_index = (is_variadic && param_count > 0) ? param_count - 1 : -1;
    int fixed_param_count = rest_param_index >= 0 ? rest_param_index : param_count;
    if (!fn_links && call->callee && call->callee->type == AST_MEMBER_ACCESS)
        fn_links = xa_static_method_fn_links(ctx, call->callee);
    bool is_mem_addr = xa_call_is_mem_addr(call, fn_links);

    // Caller-side default argument filling (C1): for a direct call to a named
    // function with default parameters, complete omitted trailing arguments by
    // appending session-cloned copies of the declared default expressions. This
    // makes defaults evaluated at the call site instead of via a runtime null
    // sentinel, so passing an explicit `null` is preserved (not treated as
    // omitted). Indirect/function-value calls carry no default expressions and
    // therefore must pass every argument.
    if (fn_links && fn_links->param_defaults && !is_variadic &&
        fn_links->param_count == param_count) {
        if (call->arg_count < param_count) {
            xa_complete_call_default_args(ctx, call, fn_links, param_count);
        } else if (call->arg_count == param_count) {
            xa_mark_call_default_arg_contract(call, fn_links, param_count);
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
    XrParamMode *effective_arg_modes = NULL;
    char (*effective_arg_paths)[XA_CALL_ALIAS_PATH_MAX] = NULL;
    bool *effective_arg_path_precise = NULL;
    if (arg_count > 0) {
        effective_arg_symbol_ids = (uint32_t *) xr_calloc((size_t) arg_count, sizeof(uint32_t));
        effective_arg_names = (const char **) xr_calloc((size_t) arg_count, sizeof(const char *));
        effective_arg_modes = (XrParamMode *) xr_calloc((size_t) arg_count, sizeof(XrParamMode));
        effective_arg_paths = (char (*)[XA_CALL_ALIAS_PATH_MAX]) xr_calloc(
            (size_t) arg_count, sizeof(*effective_arg_paths));
        effective_arg_path_precise = (bool *) xr_calloc((size_t) arg_count, sizeof(bool));
    }

    // Check argument count (use min_params for functions with default parameters)
    int min_params = callee_type->function.min_params;
    if (arg_count < min_params) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        if (is_variadic) {
            snprintf(msg, sizeof(msg), "Expected at least %d argument(s), but got %d", min_params,
                     arg_count);
        } else if (min_params == param_count) {
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
    } else if (!is_variadic && arg_count < param_count && min_params < param_count &&
               call->callee && call->callee->type == AST_VARIABLE && fn_sym &&
               fn_sym->kind != XA_SYM_FUNCTION) {
        // Default arguments are filled at the call site only for direct calls
        // to a named function (C1). A call through a function-typed *value*
        // (a `var` binding, parameter, capture, ...) carries no default
        // expressions, so every argument must be passed. The symbol-kind
        // check is the discriminator: a named function resolves to
        // XA_SYM_FUNCTION (its defaults were spliced in above when
        // available), while a function-typed value resolves to a variable
        // symbol whose links carry no param_defaults. Builtins resolve to
        // no symbol at all and keep their optional-argument semantics.
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
    const char *math_member = xa_math_call_member(ctx, call, fn_links);
    bool math_preserves_numeric_shape =
        math_member && (strcmp(math_member, "abs") == 0 || strcmp(math_member, "min") == 0 ||
                        strcmp(math_member, "max") == 0 || strcmp(math_member, "clamp") == 0);
    bool math_first_arg_seen = false;
    bool math_int_shape = false;
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
                int param_slot = slot;
                if (is_variadic && rest_param_index >= 0 && slot >= fixed_param_count)
                    param_slot = rest_param_index;
                if (effective_arg_types && slot < arg_count)
                    effective_arg_types[slot] = arg_type;
                if (effective_arg_modes && slot < arg_count)
                    effective_arg_modes[slot] = xa_call_param_mode(callee_type, param_slot);
                xa_check_channel_send_transfer_arg(ctx, node, callee_obj_type, method_name,
                                                   arg_node, arg_type, slot);
                if (param_slot < 0 || param_slot >= param_count)
                    continue;
                XrParamMode param_mode = xa_call_param_mode(callee_type, param_slot);
                if (!(slot == 0 &&
                      xa_is_channel_send_boundary_method(callee_obj_type, method_name) &&
                      xa_call_arg_access(call, i) == XR_CALL_ARG_MOVE)) {
                    xa_check_call_arg_access_authorization(ctx, node, call, arg_node, i, slot,
                                                           param_mode);
                }
                XrType *param_type = xr_type_function_param_type(callee_type, param_slot);
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
                if (!null_err && !xa_call_arg_type_assignable(param_type, arg_type, param_mode) &&
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

        int param_slot = slot;
        if (is_variadic && rest_param_index >= 0 && slot >= fixed_param_count)
            param_slot = rest_param_index;

        if (param_slot < 0 || param_slot >= param_count) {
            const char *parallel_callback_label =
                xa_parallel_callback_label_for_plan(parallel_call_plan, i);
            XrCallArgAccess access = xa_call_arg_access(call, i);
            XrType *arg_type = NULL;
            if (access != XR_CALL_ARG_PLAIN) {
                arg_type = xa_call_direct_variable_type_without_read(ctx, arg_node);
                xa_report_arg_access_without_matching_contract(ctx, node, arg_node, access, slot);
            }
            if (!arg_type)
                arg_type =
                    xa_visit_call_arg_with_parallel_context(ctx, arg_node, parallel_callback_label);
            if (effective_arg_types && slot < arg_count)
                effective_arg_types[slot] = arg_type;
            if (effective_arg_modes && slot < arg_count)
                effective_arg_modes[slot] = xa_call_param_mode(callee_type, param_slot);
            slot++;
            continue;
        }

        XrType *declared_param_type = xr_type_function_param_type(callee_type, param_slot);
        /* The hazard this guards against is a *user declaration elsewhere in the
         * program* silently retyping a caller's literal.  Two parameter kinds
         * cannot do that: a type parameter carries whatever the receiver or the
         * call site already stated, and a built-in member's signature is fixed
         * by the language, so `arrayOfJson.push({...})` states its domain
         * through the receiver's own annotation. */
        bool param_type_is_generic =
            (declared_param_type && declared_param_type->kind == XR_KIND_TYPE_PARAM) ||
            (method_name && callee_obj_type && xa_builtin_is_method(callee_obj_type, method_name));
        XrType *param_type = xa_contextualize_generic_call_param(
            ctx, fn_links, callee_type, call, effective_arg_types, slot, declared_param_type);
        XrType *saved_expected = ctx->expected_type;
        bool saved_copy_view = ctx->allow_view_expr_for_copy;
        if (math_preserves_numeric_shape && !math_first_arg_seen) {
            ctx->expected_type = NULL;
        } else if (math_preserves_numeric_shape && math_int_shape) {
            ctx->expected_type = xr_type_new_int(ctx->analyzer->isolate);
        } else if (param_type && !XR_TYPE_IS_UNKNOWN(param_type)) {
            ctx->expected_type = param_type;
        }
        XrType *saved_from_signature = ctx->expected_from_signature;
        if (!param_type_is_generic)
            ctx->expected_from_signature = ctx->expected_type;
        if (xa_call_is_copy_builtin(call) && slot == 0)
            ctx->allow_view_expr_for_copy = true;
        const char *parallel_callback_label =
            xa_parallel_callback_label_for_plan(parallel_call_plan, i);
        XrParamMode param_mode = xa_call_param_mode(callee_type, param_slot);
        XrCallArgAccess access = xa_call_arg_access(call, i);
        XrType *arg_type = xa_visit_call_arg_for_param_mode(ctx, arg_node, parallel_callback_label,
                                                            access, param_mode);
        ctx->allow_view_expr_for_copy = saved_copy_view;
        ctx->expected_type = saved_expected;
        ctx->expected_from_signature = saved_from_signature;
        if (math_preserves_numeric_shape && !math_first_arg_seen) {
            math_first_arg_seen = true;
            math_int_shape = arg_type && XR_TYPE_IS_INT(arg_type);
        }
        if (math_preserves_numeric_shape && math_int_shape)
            param_type = xr_type_new_int(ctx->analyzer->isolate);
        if (effective_arg_types && slot < arg_count)
            effective_arg_types[slot] = arg_type;
        xa_check_channel_send_transfer_arg(ctx, node, callee_obj_type, method_name, arg_node,
                                           arg_type, slot);
        if (effective_arg_modes && slot < arg_count)
            effective_arg_modes[slot] = param_mode;
        if (!(slot == 0 && xa_is_channel_send_boundary_method(callee_obj_type, method_name) &&
              access == XR_CALL_ARG_MOVE)) {
            xa_check_call_arg_access_authorization(ctx, node, call, arg_node, i, slot, param_mode);
        }
        xa_check_ref_argument_not_readonly(ctx, node, arg_node, slot, param_mode);
        if (param_mode == XR_PARAM_READ || param_mode == XR_PARAM_REF ||
            param_mode == XR_PARAM_MOVE) {
            bool path_precise = false;
            char local_path[XA_CALL_ALIAS_PATH_MAX];
            XaSymbol *arg_sym = xa_call_alias_path_symbol(ctx, arg_node, local_path,
                                                          sizeof(local_path), &path_precise);
            if (!arg_sym)
                arg_sym = xa_call_root_variable_symbol(ctx, arg_node);
            if (arg_sym && param_mode == XR_PARAM_REF)
                arg_sym->links.value_mutated = true;
            if (arg_sym && effective_arg_symbol_ids && effective_arg_names && slot < arg_count) {
                effective_arg_symbol_ids[slot] = arg_sym->id;
                effective_arg_names[slot] = arg_sym->name;
                if (effective_arg_paths && effective_arg_path_precise) {
                    xa_call_alias_path_copy(effective_arg_paths[slot], XA_CALL_ALIAS_PATH_MAX,
                                            local_path[0] ? local_path
                                                          : (arg_sym->name ? arg_sym->name : ""));
                    effective_arg_path_precise[slot] = path_precise && local_path[0] != '\0';
                }
            }
        }

        if (is_mem_addr && slot == 0) {
            xa_check_mem_addr_arg(ctx, arg_node, arg_type);
            xa_check_pointer_borrow_escape(ctx, arg_node, arg_node, arg_type,
                                           "erase raw pointer borrow provenance with mem.addr");
            slot++;
            continue;
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
            if (!null_err && !xa_call_arg_type_assignable(param_type, arg_type, param_mode) &&
                !xr_is_json_coercion(param_type, arg_type)) {
                char msg[512];
                if (xr_type_is_enum_metadata_named(arg_type, XR_ENUM_VARIANT_TYPE_NAME) &&
                    param_type->kind == XR_KIND_ENUM) {
                    XrType *owner = xr_type_enum_metadata_owner(arg_type);
                    snprintf(
                        msg, sizeof(msg),
                        "Argument %d: type '%s' is not assignable to parameter type '%s'; "
                        "'%s.variants' yields variant descriptors, so iterate '%s' directly "
                        "when actual enum values are required",
                        slot + 1, xr_type_to_string(arg_type), xr_type_to_string(param_type),
                        owner && owner->enum_type.enum_name ? owner->enum_type.enum_name : "Enum",
                        param_type->enum_type.enum_name ? param_type->enum_type.enum_name : "Enum");
                } else {
                    char reason[192];
                    if (xr_type_record_mismatch_reason(param_type, arg_type, reason,
                                                       sizeof(reason))) {
                        snprintf(
                            msg, sizeof(msg),
                            "Argument %d: type '%s' is not assignable to parameter type '%s'; %s",
                            slot + 1, xr_type_to_string(arg_type), xr_type_to_string(param_type),
                            reason);
                    } else {
                        snprintf(msg, sizeof(msg),
                                 "Argument %d: type '%s' is not assignable to parameter type '%s'",
                                 slot + 1, xr_type_to_string(arg_type),
                                 xr_type_to_string(param_type));
                    }
                }
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
            }
        }
        slot++;
    }
    xa_check_ref_argument_aliases(ctx, node, effective_arg_symbol_ids, effective_arg_names,
                                  effective_arg_modes, effective_arg_paths,
                                  effective_arg_path_precise, arg_count);
    XaSymbolLinks *escape_links = fn_links;
    const char *escape_name = NULL;
    if (call->callee && call->callee->type == AST_VARIABLE)
        escape_name = call->callee->as.variable.name;
    if (method_name && callee_obj_type) {
        if (!escape_links)
            escape_links = xa_method_symbol_links_for_call(ctx, callee_obj_type, method_name);
        escape_name = method_name;
    }
    const XaMemoryEffectSummary *call_memory_effects =
        xa_symbol_links_memory_effect_summary(escape_links);
    if (method_name && call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        xa_check_call_memory_effect_actual(ctx, node, call_memory_effects, XA_MEMORY_ROOT_RECEIVER,
                                           0, call->callee->as.member_access.object, escape_name);
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
                                   : NULL;
            if (!arg_type) {
                XrParamMode param_mode = xa_call_param_mode(callee_type, direct_slot);
                XrCallArgAccess access = xa_call_arg_access(call, i);
                arg_type =
                    xa_visit_call_arg_for_param_mode(ctx, arg_node, NULL, access, param_mode);
            }
            xa_check_transfer_storage_param_arg(ctx, node, escape_links, escape_name, arg_node,
                                                arg_type, direct_slot);
            xa_check_borrowed_escaping_param_arg(ctx, node, escape_links, escape_name, arg_node,
                                                 arg_type, direct_slot);
            xa_check_shared_mutating_param_arg(ctx, node, escape_links, escape_name, arg_node,
                                               arg_type, direct_slot);
            xa_check_call_memory_effect_actual(ctx, node, call_memory_effects, XA_MEMORY_ROOT_PARAM,
                                               (uint32_t) direct_slot, arg_node, escape_name);
            if (!call_memory_effects) {
                xa_check_unknown_function_value_memory_effect_actual(
                    ctx, node, call, fn_sym, fn_links, callee_type, arg_node, direct_slot,
                    escape_name);
            }
            direct_slot++;
        }
    }
    if (call->arg_count > 0) {
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
                                   : NULL;
            if (!arg_type) {
                XrParamMode param_mode = xa_call_param_mode(callee_type, direct_slot);
                XrCallArgAccess access = xa_call_arg_access(call, i);
                arg_type =
                    xa_visit_call_arg_for_param_mode(ctx, arg_node, NULL, access, param_mode);
            }
            xa_check_shared_unknown_function_value_arg(
                ctx, node, call, fn_sym, fn_links, callee_type, arg_node, arg_type, direct_slot);
            xa_check_pointer_unknown_function_value_arg(
                ctx, node, call, fn_sym, fn_links, callee_type, arg_node, arg_type, direct_slot);
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
                                   : NULL;
            if (!arg_type) {
                XrParamMode param_mode = xa_call_param_mode(callee_type, direct_slot);
                XrCallArgAccess access = xa_call_arg_access(call, i);
                arg_type =
                    xa_visit_call_arg_for_param_mode(ctx, arg_node, NULL, access, param_mode);
            }
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

    xa_check_freestanding_math_call(ctx, node, call, fn_links, effective_arg_types, arg_count);

    XrType *return_type = callee_type->function.return_type;

    if (method_name && callee_obj_type) {
        XrType *builtin_return_type =
            xa_builtin_get_method_return_type(ctx->analyzer->isolate, callee_obj_type, method_name);
        if (builtin_return_type &&
            (!return_type || XR_TYPE_IS_UNKNOWN(return_type) || XR_TYPE_IS_JSON(return_type))) {
            return_type = builtin_return_type;
        }
    }

    /* R2-2: wrapping* on a fixed-width int receiver is width-lowered to the
     * arithmetic ops, so the result keeps the receiver's width
     * (i32.wrappingAdd -> i32), not the plain `int` from the native
     * class signature. */
    if (method_name && callee_obj_type && XR_TYPE_IS_INT(callee_obj_type) &&
        !callee_obj_type->is_nullable && callee_obj_type->scalar_rep != XR_NATIVE_I64 &&
        (strcmp(method_name, "wrappingAdd") == 0 || strcmp(method_name, "wrappingSub") == 0 ||
         strcmp(method_name, "wrappingMul") == 0)) {
        return_type = callee_obj_type;
    }

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

    if (parallel_call_plan && parallel_call_plan->is_plan_method) {
        switch (parallel_call_plan->kind) {
            case XA_PAR_CALL_FOR_EACH:
                return_type = xr_type_new_unit(ctx->analyzer->isolate);
                break;
            case XA_PAR_CALL_MAP: {
                XrType *callback_type =
                    call->arg_count > 1
                        ? xa_analyzer_get_node_type(ctx->analyzer, call->arguments[1])
                        : NULL;
                XrType *element_type = callback_type && XR_TYPE_IS_FUNCTION(callback_type)
                                           ? callback_type->function.return_type
                                           : xr_type_new_unknown(ctx->analyzer->isolate);
                return_type = xr_type_new_array(ctx->analyzer->isolate, element_type);
                break;
            }
            case XA_PAR_CALL_MAP_INTO:
                if (call->arg_count > 1)
                    return_type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[1]);
                break;
            case XA_PAR_CALL_REDUCE:
                if (call->arg_count > 1)
                    return_type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[1]);
                break;
            case XA_PAR_CALL_NONE:
                break;
        }
    }

    // Apply type substitution for generic function calls. Write inferred type
    // args back only for locally-defined generic functions; imported ones (e.g.
    // parallel.reduce) are lowered by dedicated cgen intrinsics that require the
    // call to remain in inferred (type_arg_count == 0) form.
    if (return_type && fn_links) {
        return_type =
            xa_substitute_generic_call(ctx, fn_links, callee_type, return_type, call, arg_count,
                                       effective_arg_types, fn_sym && !fn_sym->is_imported);
    }

    XrType *math_shape_type =
        xa_math_runtime_shape_return_type(ctx, call, fn_links, effective_arg_types, arg_count);
    if (math_shape_type)
        return_type = math_shape_type;

    /* copy(x): the deep-copy builtin is an identity over types and must
     * preserve the argument's static type. It is registered as any->any, so
     * without this the result is unknown and cannot be passed to a typed
     * parameter (breaks `shared c = copy(obj)` feeding a typed callee).
     * Guard on unknown so a user-defined copy with a concrete signature wins. */
    if (xa_call_is_copy_builtin(call) && arg_count == 1 && effective_arg_types &&
        effective_arg_types[0] && (!return_type || XR_TYPE_IS_UNKNOWN(return_type))) {
        XrType *owned = xa_copy_owned_return_type(ctx, effective_arg_types[0]);
        return_type = owned ? owned : effective_arg_types[0];
    }
    if (xa_freestanding_profile_enabled(ctx->analyzer) && xa_call_is_copy_builtin(call) &&
        arg_count == 1 && effective_arg_types && effective_arg_types[0] &&
        !xa_fixed_value_copy_is_no_heap(effective_arg_types[0], 0)) {
        xa_freestanding_report_unavailable(
            ctx, node, "builtin copy()",
            "freestanding copy supports only scalar, pointer, and recursively fixed-array "
            "values");
    }

    if (xa_call_is_byte_slice_typed_load(call, callee_obj_type))
        return_type = xa_load_le_return_type(ctx, node, call, "Slice<byte>.load<T>()", true);

    if (xa_call_is_byte_slice_typed_store(call, callee_obj_type)) {
        (void) xa_byte_slice_typed_type_arg(ctx, node, call, "Slice<byte>.store<T>()", true);
        return_type = xr_type_new_unit(ctx->analyzer->isolate);
    }

    if (xa_call_is_byte_slice_reinterpret(call, callee_obj_type))
        return_type = xa_byte_slice_reinterpret_return_type(ctx, node, call);

    // Apply type substitution for generic method calls: obj.method<T>()
    if (callee_obj_type && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;

        // Look up method in class
        if (XR_TYPE_IS_INSTANCE(callee_obj_type) && callee_obj_type->instance.class_name) {
            XaSymbol *class_sym =
                xa_lookup_visible_class_symbol(ctx, callee_obj_type->instance.class_name);
            XaSymbolLinks *class_links = (class_sym && class_sym->kind == XA_SYM_CLASS)
                                             ? xa_analyzer_get_links(ctx->analyzer, class_sym)
                                             : NULL;
            XrClassInfo *class_info = (class_links && class_links->class_info)
                                          ? class_links->class_info
                                          : callee_obj_type->instance.class_ref;
            if (class_info) {
                XaSymbol *method_sym = xa_class_info_lookup_instance_member(class_info, ma->name);
                if (method_sym && method_sym->kind == XA_SYM_METHOD) {
                    XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
                    if (method_links) {
                        // Apply method's own type parameters. Method-call inferred
                        // writeback is not needed for the gap C shapes (they rely
                        // on class-construction writeback plus mono's method-body
                        // fixpoint), so keep it disabled here.
                        return_type =
                            xa_substitute_generic_call(ctx, method_links, callee_type, return_type,
                                                       call, arg_count, effective_arg_types, false);

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

    XrType *final_type = return_type ? return_type : xr_type_new_unknown(NULL);
    if (optional_function_call && final_type && !XR_TYPE_IS_UNKNOWN(final_type))
        final_type = xr_type_make_nullable(ctx->analyzer->isolate,
                                           xr_type_copy(ctx->analyzer->isolate, final_type));
    xr_free(effective_arg_types);
    xr_free(effective_arg_symbol_ids);
    xr_free(effective_arg_names);
    xr_free(effective_arg_modes);
    xr_free(effective_arg_paths);
    xr_free(effective_arg_path_precise);
    return final_type;
}
