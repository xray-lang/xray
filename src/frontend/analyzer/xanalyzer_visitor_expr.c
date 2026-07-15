/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_expr.c - Pass 2 expression type inference visitors
 *
 * KEY CONCEPT:
 *   Type inference for all expression kinds: literals, variables,
 *   operators, calls, member access, match expressions, optional
 *   chains, closures, and generic substitution.
 */

#include "xanalyzer_visitor_internal.h"
#include "xtype_ref_resolve.h"
#include "xa_selection.h"
#include "xanalyzer_mono.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../module/xmodule_graph.h"
#include "../../base/xchecks.h"
#include "../../base/xhashmap.h"
#include "../../../stdlib/prelude/prelude.h"
#include "../../runtime/value/xtype_names.h"
#include <limits.h>
#include <stdint.h>

/* Record a selection fact for a member/index access node. */
static void record_selection(XaInferContext *ctx, AstNode *node, XaSelectionKind kind,
                             XrType *receiver, XaSymbol *target, int32_t field_idx, XrType *result,
                             bool is_optional) {
    XaSelectionTable *st = (XaSelectionTable *) ctx->analyzer->selection_table;
    if (!st)
        return;
    XaSelection sel = {
        .kind = kind,
        .receiver_type = receiver,
        .target_symbol = target,
        .field_index = field_idx,
        .result_type = result,
        .is_indirect = false,
        .is_optional = is_optional,
    };
    xa_selection_table_set(st, node, &sel);
}

static const char *object_shape_type_label(XrType *type) {
    if (XR_TYPE_HAS_OBJECT_SHAPE(type) && type->object.type_name)
        return type->object.type_name;
    return xr_type_to_string(type);
}

static int object_shape_field_index(XrType *type, const char *name) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || !name || !type->object.field_names)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names[i] && strcmp(type->object.field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static XrType *class_info_field_type(XaInferContext *ctx, XrClassInfo *info, const char *name) {
    if (!ctx || !info || !name)
        return NULL;
    XaSymbol *field = xa_class_info_lookup_member(info, name);
    XaSymbolLinks *links = field ? xa_analyzer_get_links(ctx->analyzer, field) : NULL;
    if (!links)
        return NULL;
    return links->type ? links->type : links->declared_type;
}

static bool xa_type_is_u8_array_type(XrType *type) {
    return xr_type_is_u8_array(type);
}

static bool xa_type_is_u8_slice_type(XrType *type) {
    return xr_type_is_u8_span(type);
}

static bool xa_type_is_pod_span_elem(XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool xa_type_is_raw_u8_ptr(XrType *type) {
    return xr_type_is_u8_pointer(type);
}

static XrType *xa_freestanding_reject_owned_static_member(XaInferContext *ctx, AstNode *object,
                                                          const char *name, AstNode *node) {
    if (!ctx || !object || object->type != AST_VARIABLE || !name ||
        !xa_freestanding_profile_enabled(ctx->analyzer))
        return NULL;

    const char *type_name = object->as.variable.name;
    if (!type_name)
        return NULL;
    if (strcmp(type_name, "Array") != 0 && strcmp(type_name, "StringBuilder") != 0)
        return NULL;

    char feature[160];
    snprintf(feature, sizeof(feature), "%s.%s", type_name, name);
    xa_freestanding_report_unavailable(
        ctx, node ? node : object, feature,
        "owned heap-backed containers are not part of the freestanding no-heap subset");
    return xr_type_new_error(ctx->analyzer->isolate);
}

static bool xa_freestanding_reject_string_member(XaInferContext *ctx, AstNode *node,
                                                 XrType *receiver, const char *name) {
    if (!ctx || !XR_TYPE_IS_STRING(receiver) || !name ||
        !xa_freestanding_profile_enabled(ctx->analyzer))
        return false;

    char feature[160];
    snprintf(feature, sizeof(feature), "string.%s", name);
    xa_freestanding_report_unavailable(
        ctx, node, feature,
        "string literals may be passed or printed, but string member access needs hosted helpers");
    return true;
}

static bool xa_array_repeat_count_const_expr(XaInferContext *ctx, AstNode *node, int *out,
                                             const char **out_error) {
    if (out_error)
        *out_error = NULL;
    if (!ctx || !ctx->analyzer || !node || !out)
        return false;
    int64_t value = 0;
    const char *err = NULL;
    if (!xa_eval_const_int_expr(ctx->analyzer, node, &value, &err)) {
        if (out_error)
            *out_error = err;
        return false;
    }
    if (value <= 0) {
        if (out_error)
            *out_error = "repeat count must be greater than zero";
        return false;
    }
    if (value > UINT16_MAX) {
        if (out_error)
            *out_error = "repeat count exceeds maximum of 65535 elements";
        return false;
    }
    *out = (int) value;
    return true;
}

static bool xa_symbol_is_collection_length(SymbolId sym, XrType *type) {
    (void) sym;
    (void) type;
    return false;
}

static bool xa_type_has_len_query(XrType *type) {
    return type &&
           (XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_VIEW(type) || XR_TYPE_IS_SPAN(type) ||
            XR_TYPE_IS_STRING(type) || XR_TYPE_IS_MAP(type) || type->kind == XR_KIND_SET ||
            type->kind == XR_KIND_FIXED_ARRAY || type->kind == XR_KIND_CHANNEL ||
            xr_type_is_named_class(type, "StringBuilder") ||
            xr_type_is_named_class(type, "Buffer") || xr_type_is_named_class(type, "WorkQueue") ||
            xr_type_is_named_class(type, "ResultGroup"));
}

static void xa_report_span_member_error(XaInferContext *ctx, AstNode *node, XrType *type,
                                        const char *name) {
    if (!ctx || !ctx->analyzer || !node || !type || !XR_TYPE_IS_SPAN(type) || !name)
        return;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[192];
    snprintf(msg, sizeof(msg), "Slice view has no member '%s'; use len(view) or indexed access",
             name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_report_view_member_error(XaInferContext *ctx, AstNode *node, XrType *type,
                                        const char *name) {
    if (!ctx || !ctx->analyzer || !node || !type || !XR_TYPE_IS_VIEW(type) || !name)
        return;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[192];
    snprintf(msg, sizeof(msg), "View has no member '%s'; use len(view) or indexed access", name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static bool member_object_is_enum_namespace(XaInferContext *ctx, AstNode *object,
                                            const char *enum_name) {
    if (!ctx || !object || object->type != AST_VARIABLE || !enum_name)
        return false;
    XaSymbol *sym =
        object->as.variable.symbol_id
            ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, object->as.variable.symbol_id)
            : NULL;
    if (sym && sym->kind == XA_SYM_ENUM && sym->name && strcmp(sym->name, enum_name) == 0)
        return true;
    const char *name = object->as.variable.name;
    if (!name || strcmp(name, enum_name) != 0)
        return false;
    sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
    return sym && sym->kind == XA_SYM_ENUM;
}

static XaSymbol *xa_expected_enum_symbol(XaInferContext *ctx, AstNode *object) {
    if (!ctx || !ctx->analyzer || !ctx->expected_type || ctx->expected_type->kind != XR_KIND_ENUM ||
        !ctx->expected_type->enum_type.enum_name || !object || object->type != AST_VARIABLE ||
        !object->as.variable.name)
        return NULL;

    const char *enum_name = ctx->expected_type->enum_type.enum_name;
    if (strcmp(object->as.variable.name, enum_name) != 0)
        return NULL;

    XaSymbol *sym =
        object->as.variable.symbol_id
            ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, object->as.variable.symbol_id)
            : NULL;
    if (!sym)
        sym = xa_analyzer_lookup(ctx->analyzer, enum_name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(ctx->analyzer, enum_name, ctx->analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(ctx->analyzer, enum_name);
    if (!sym && ctx->analyzer->graph) {
        XrModuleGraph *graph = (XrModuleGraph *) ctx->analyzer->graph;
        XaSymbol *found = NULL;
        uint32_t expected_layout_id = ctx->expected_type->enum_type.layout_id;
        for (int i = 0; i < graph->spec_count; i++) {
            XrHashMap *exports = graph->specs[i].export_symbols;
            XaSymbol *candidate = exports ? (XaSymbol *) xr_hashmap_get(exports, enum_name) : NULL;
            if (!candidate || candidate->kind != XA_SYM_ENUM)
                continue;
            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, candidate);
            XaEnumInfo *info = links ? links->enum_info : NULL;
            uint32_t candidate_layout_id =
                info && info->layout
                    ? info->layout->layout_id
                    : (links && links->type ? links->type->enum_type.layout_id : 0);
            if (expected_layout_id != 0 && candidate_layout_id != 0 &&
                expected_layout_id != candidate_layout_id)
                continue;
            if (found && found != candidate)
                return NULL;
            found = candidate;
        }
        sym = found;
    }
    return sym && sym->kind == XA_SYM_ENUM ? sym : NULL;
}

static XrType *xa_try_expected_enum_member_access(XaInferContext *ctx, AstNode *node,
                                                  MemberAccessNode *ma) {
    XaSymbol *enum_sym = xa_expected_enum_symbol(ctx, ma ? ma->object : NULL);
    if (!enum_sym || !ma || !ma->name)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, enum_sym);
    XaEnumInfo *info = links ? links->enum_info : NULL;
    int member_index = xa_enum_info_find_variant(info, ma->name);
    if (member_index < 0)
        return NULL;

    ma->object->as.variable.symbol_id = enum_sym->id;
    if (info->variants && info->variants[member_index].payload_count > 0 &&
        !ctx->allow_payload_enum_ctor_value) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "payload enum variant '%s.%s' is a constructor; call it as '%s.%s(...)' "
                 "instead of using it as a value",
                 enum_sym->name ? enum_sym->name : "?", ma->name,
                 enum_sym->name ? enum_sym->name : "?", ma->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    XrType *enum_type =
        xr_type_new_enum(ctx->analyzer->isolate, ctx->expected_type->enum_type.enum_name);
    if (enum_type) {
        enum_type->enum_type.layout = info ? info->layout : ctx->expected_type->enum_type.layout;
        enum_type->enum_type.layout_id = enum_type->enum_type.layout
                                             ? enum_type->enum_type.layout->layout_id
                                             : ctx->expected_type->enum_type.layout_id;
    }
    record_selection(ctx, node, XA_SEL_ENUM_MEMBER, enum_type ? enum_type : ctx->expected_type,
                     enum_sym, member_index, enum_type ? enum_type : ctx->expected_type, false);
    return enum_type ? enum_type : ctx->expected_type;
}

static XrType *xa_function_type1(XaInferContext *ctx, XrType *p0, XrType *ret) {
    XrType *params[1] = {p0};
    return xr_type_new_function(ctx->analyzer->isolate, params, 1, ret, false);
}

#include "xbuiltin_receiver_registry.h"

static bool xa_builtin_receiver_matches(XrType *receiver, XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver && XR_TYPE_IS_ARRAY(receiver);
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver && XR_TYPE_IS_SPAN(receiver) && receiver->container.element_type &&
                   xa_type_is_pod_span_elem(receiver->container.element_type);
    }
    return false;
}

static const XaBuiltinReceiverMethodSpec *xa_find_builtin_receiver_method_spec(XrType *receiver,
                                                                               const char *name) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (strcmp(spec->source_name, name) == 0 &&
            xa_builtin_receiver_matches(receiver, spec->receiver))
            return spec;
    }
    return NULL;
}

static const char *xa_builtin_receiver_display_name(const XaBuiltinReceiverMethodSpec *spec,
                                                    XrType *receiver) {
    if (!spec)
        return "receiver";
    switch (spec->receiver) {
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return "Array<byte>";
        case XA_BUILTIN_RECEIVER_ARRAY:
            return xa_type_is_u8_array_type(receiver) ? "Array<byte>" : "Array";
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return "Slice<byte>";
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return xa_type_is_u8_slice_type(receiver) ? "Slice<byte>" : "Slice";
    }
    return "receiver";
}

static XrType *xa_builtin_method_component_type(XaInferContext *ctx, XaBuiltinMethodTypeKind kind,
                                                XrType *receiver, XrType *type_param0) {
    XrVMRuntime *X = ctx->analyzer->isolate;
    switch (kind) {
        case XA_BUILTIN_TYPE_NONE:
            return NULL;
        case XA_BUILTIN_TYPE_BOOL:
            return xr_type_new_bool(X);
        case XA_BUILTIN_TYPE_INT:
            return xr_type_new_int(X);
        case XA_BUILTIN_TYPE_STRING:
            return xr_type_new_string(X);
        case XA_BUILTIN_TYPE_U8:
            return xr_type_new_int_width(X, XR_NATIVE_U8);
        case XA_BUILTIN_TYPE_U8_ARRAY:
            return xr_type_new_u8_array(X);
        case XA_BUILTIN_TYPE_U8_SLICE:
            return xr_type_new_u8_slice(X);
        case XA_BUILTIN_TYPE_UNIT:
            return xr_type_new_unit(X);
        case XA_BUILTIN_TYPE_ENDIAN:
            return xr_type_new_enum(X, "Endian");
        case XA_BUILTIN_TYPE_PARAM_0:
            return type_param0 ? type_param0 : xr_type_new_type_param(X, "T", 0);
        case XA_BUILTIN_TYPE_ARRAY_OF_PARAM_0:
            return xr_type_new_array(X,
                                     type_param0 ? type_param0 : xr_type_new_type_param(X, "T", 0));
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_TO_BOOL_FN: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *params[1] = {elem};
            return xr_type_new_function(X, params, 1, xr_type_new_bool(X), false);
        }
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_BOOL_FN: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *params[2] = {elem, xr_type_new_int(X)};
            return xr_type_new_function(X, params, 2, xr_type_new_bool(X), false);
        }
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_UNIT_FN: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *params[2] = {elem, xr_type_new_int(X)};
            return xr_type_new_function(X, params, 2, xr_type_new_unit(X), false);
        }
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *params[2] = {elem, xr_type_new_int(X)};
            return xr_type_new_function(
                X, params, 2, type_param0 ? type_param0 : xr_type_new_type_param(X, "T", 0), false);
        }
        case XA_BUILTIN_TYPE_PARAM_0_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN: {
            XrType *acc = type_param0 ? type_param0 : xr_type_new_type_param(X, "T", 0);
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *params[3] = {acc, elem, xr_type_new_int(X)};
            return xr_type_new_function(X, params, 3, acc, false);
        }
        case XA_BUILTIN_TYPE_SLICE_OF_PARAM_0:
            return xr_type_new_span(X,
                                    type_param0 ? type_param0 : xr_type_new_type_param(X, "T", 0));
        case XA_BUILTIN_TYPE_RECEIVER:
            return receiver ? receiver : xr_type_new_unknown(X);
        case XA_BUILTIN_TYPE_RECEIVER_ELEM:
            return receiver && receiver->container.element_type ? receiver->container.element_type
                                                                : xr_type_new_unknown(X);
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_NULLABLE: {
            XrType *elem = receiver && receiver->container.element_type
                               ? xr_type_copy(X, receiver->container.element_type)
                               : xr_type_new_unknown(X);
            return xr_type_make_nullable(X, elem);
        }
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_COMPARE_FN: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *params[2] = {elem, elem};
            return xr_type_new_function(X, params, 2, xr_type_new_int(X), false);
        }
        case XA_BUILTIN_TYPE_ITERATOR_OF_RECEIVER_ELEM: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *args[1] = {elem};
            return xr_type_new_generic_instance(X, "Iterator", NULL, args, 1);
        }
        case XA_BUILTIN_TYPE_ITERATOR_OF_INDEX_RECEIVER_ELEM_TUPLE: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *tuple_elems[2] = {xr_type_new_int(X), elem};
            XrType *pair = xr_type_new_tuple(X, tuple_elems, 2);
            XrType *args[1] = {pair ? pair : xr_type_new_unknown(X)};
            return xr_type_new_generic_instance(X, "Iterator", NULL, args, 1);
        }
        case XA_BUILTIN_TYPE_ARRAY_OF_INDEX_RECEIVER_ELEM_TUPLE: {
            XrType *elem = receiver && receiver->container.element_type
                               ? receiver->container.element_type
                               : xr_type_new_unknown(X);
            XrType *tuple_elems[2] = {xr_type_new_int(X), elem};
            XrType *pair = xr_type_new_tuple(X, tuple_elems, 2);
            return xr_type_new_array(X, pair ? pair : xr_type_new_unknown(X));
        }
        case XA_BUILTIN_TYPE_SLICE_OF_RECEIVER_ELEM:
            return xr_type_new_span(X, receiver && receiver->container.element_type
                                           ? receiver->container.element_type
                                           : xr_type_new_unknown(X));
        case XA_BUILTIN_TYPE_PTR_OF_RECEIVER_ELEM:
            return xr_type_new_pointer(X,
                                       receiver && receiver->container.element_type
                                           ? receiver->container.element_type
                                           : xr_type_new_unknown(X),
                                       false);
        case XA_BUILTIN_TYPE_MUT_PTR_OF_RECEIVER_ELEM:
            return xr_type_new_pointer(X,
                                       receiver && receiver->container.element_type
                                           ? receiver->container.element_type
                                           : xr_type_new_unknown(X),
                                       true);
    }
    return xr_type_new_unknown(X);
}

static XrType *xa_builtin_receiver_method_type_from_spec(XaInferContext *ctx, XrType *receiver,
                                                         const XaBuiltinReceiverMethodSpec *spec) {
    if (!ctx || !ctx->analyzer || !spec)
        return NULL;
    XrVMRuntime *X = ctx->analyzer->isolate;
    const char *type_param0_name = spec->type_params == XA_BUILTIN_TYPE_PARAMS_U ? "U" : "T";
    XrType *type_param0 = spec->type_params != XA_BUILTIN_TYPE_PARAMS_NONE
                              ? xr_type_new_type_param(X, type_param0_name, 0)
                              : NULL;
    XrType *params[3] = {NULL, NULL, NULL};
    for (int p = 0; p < spec->param_count && p < 3; p++)
        params[p] = xa_builtin_method_component_type(ctx, spec->params[p], receiver, type_param0);
    XrType *ret = xa_builtin_method_component_type(ctx, spec->result, receiver, type_param0);
    XrType *fn = xr_type_new_function(X, spec->param_count > 0 ? params : NULL, spec->param_count,
                                      ret, spec->is_variadic);
    if (fn && spec->type_params != XA_BUILTIN_TYPE_PARAMS_NONE) {
        const char *names[1] = {type_param0_name};
        xr_type_set_function_type_params(X, fn, names, NULL, NULL, 1);
    }
    if (fn)
        fn->function.min_params = spec->min_params;
    return fn;
}

static void xa_report_builtin_receiver_unsafe_requirement(XaInferContext *ctx, AstNode *node,
                                                          XrType *receiver,
                                                          const XaBuiltinReceiverMethodSpec *spec) {
    if (!ctx || !ctx->analyzer || !node || !spec || ctx->unsafe_depth != 0 ||
        spec->unsafe_requirement != XA_BUILTIN_UNSAFE_REQUIRED)
        return;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[128];
    snprintf(msg, sizeof(msg), "%s.%s() must be inside an unsafe block",
             xa_builtin_receiver_display_name(spec, receiver), spec->source_name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE, msg,
                               &loc);
}

static bool xa_has_view_target_context(XaInferContext *ctx) {
    return ctx && ((ctx->expected_type && !XR_TYPE_IS_UNKNOWN(ctx->expected_type)) ||
                   ctx->allow_view_expr_for_copy);
}

static XrType *xa_string_view_method_type(XaInferContext *ctx, XrType *receiver, const char *name,
                                          AstNode *node) {
    if (!receiver || !XR_TYPE_IS_STRING(receiver) || !name)
        return NULL;
    if (strcmp(name, "bytes") != 0)
        return NULL;
    if (!xa_has_view_target_context(ctx)) {
        xa_report_view_expr_requires_target(ctx, node, "string.bytes()");
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    XrVMRuntime *X = ctx->analyzer->isolate;
    return xr_type_new_function(X, NULL, 0, xr_type_new_u8_slice(X), false);
}

static XrType *xa_array_view_method_type(XaInferContext *ctx, XrType *receiver, const char *name,
                                         AstNode *node) {
    if (!receiver || !XR_TYPE_IS_ARRAY(receiver) || !name || strcmp(name, "span") != 0)
        return NULL;
    XrLocation loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                               "Array.span() has been removed; use a target-typed slice such as "
                               "'const s: Span<T> = arr[:]'",
                               &loc);
    return xr_type_new_error(ctx->analyzer->isolate);
}

static bool xa_contextual_view_method_without_target(XaInferContext *ctx, XrType *receiver,
                                                     const char *name, AstNode *node) {
    if (!receiver || !name)
        return false;
    bool is_view_method =
        (xr_type_is_named_class(receiver, "Buffer") && strcmp(name, "asSpan") == 0) ||
        (XR_TYPE_IS_SPAN(receiver) &&
         (strcmp(name, "asBytes") == 0 || strcmp(name, "reinterpret") == 0));
    if (!is_view_method || xa_has_view_target_context(ctx))
        return false;
    xa_report_view_expr_requires_target(ctx, node, name);
    return true;
}

static XrType *xa_array_data_ptr_method_type(XaInferContext *ctx, XrType *receiver,
                                             const char *name) {
    if (!receiver || !name || (!XR_TYPE_IS_ARRAY(receiver) && !XR_TYPE_IS_SPAN(receiver)))
        return NULL;
    bool mut = false;
    if (strcmp(name, "ptr") == 0) {
        mut = false;
    } else if (strcmp(name, "mutPtr") == 0) {
        mut = true;
    } else {
        return NULL;
    }
    XrVMRuntime *X = ctx->analyzer->isolate;
    XrType *elem = receiver->container.element_type;
    if (!elem)
        elem = xr_type_new_unknown(X);
    XrType *ret = xr_type_new_pointer(X, elem, mut);
    return xr_type_new_function(X, NULL, 0, ret, false);
}

// FFI raw pointer (RawPtr<T>/RawMut<T>) instance methods. Returns the method's
// function type, or NULL if `name` is not a pointer method.
//   p.deref()    -> T          (read *p; unsafe — requires `unsafe { }`)
//   p.offset(i)  -> RawPtr<T>  (p + i, scaled by sizeof(T); safe pointer math)
//   p.isNull()   -> bool       (p == null; safe)
static XrType *xa_pointer_method_type(XaInferContext *ctx, XrType *receiver, const char *name,
                                      AstNode *node) {
    if (!receiver || !XR_TYPE_IS_POINTER(receiver) || !name)
        return NULL;
    XrVMRuntime *X = ctx->analyzer->isolate;
    XrType *pointee = receiver->container.element_type;
    if (!pointee)
        pointee = xr_type_new_unknown(X);
    if (strcmp(name, "deref") == 0) {
        // Dereference is unsafe (no null/bounds guarantee).
        if (ctx->unsafe_depth == 0 && node) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                "raw pointer `deref()` must be inside an `unsafe { }` block", &loc);
        }
        return xr_type_new_function(X, NULL, 0, pointee, false);
    }
    if (strcmp(name, "copyFromNonOverlapping") == 0) {
        if (node) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            if (ctx->unsafe_depth == 0) {
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                    "raw pointer copy must be inside an `unsafe { }` block", &loc);
            }
            if (!receiver->ptr_is_mut) {
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                    "cannot copy into a const `RawPtr<T>` (use `RawMut<T>`)", &loc);
            }
        }
        XrType *params[2] = {xr_type_new_pointer(X, pointee, false), xr_type_new_int(X)};
        return xr_type_new_function(X, params, 2, xr_type_new_unit(X), false);
    }
    if (strcmp(name, "loadLE") == 0 && xa_type_is_raw_u8_ptr(receiver)) {
        if (ctx->unsafe_depth == 0 && node) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_NOT_CALLABLE,
                                       "RawPtr.loadLE() must be inside an unsafe block", &loc);
        }
        XrType *params[1] = {xr_type_new_int(X)};
        XrType *ret = xr_type_new_type_param(X, "T", 0);
        XrType *fn = xr_type_new_function(X, params, 1, ret, false);
        if (fn) {
            const char *names[1] = {"T"};
            xr_type_set_function_type_params(X, fn, names, NULL, NULL, 1);
        }
        return fn;
    }
    if (strcmp(name, "storeLE") == 0 && xa_type_is_raw_u8_ptr(receiver)) {
        if (node) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            if (ctx->unsafe_depth == 0) {
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_NOT_CALLABLE,
                                           "RawMut.storeLE() must be inside an unsafe block", &loc);
            }
            if (!receiver->ptr_is_mut) {
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                    "cannot store through a const `RawPtr<T>` (use `RawMut<T>`)", &loc);
            }
        }
        XrType *value = xr_type_new_type_param(X, "T", 0);
        XrType *params[2] = {xr_type_new_int(X), value};
        XrType *fn = xr_type_new_function(X, params, 2, xr_type_new_unit(X), false);
        if (fn) {
            const char *names[1] = {"T"};
            xr_type_set_function_type_params(X, fn, names, NULL, NULL, 1);
        }
        return fn;
    }
    if (strcmp(name, "offset") == 0)
        return xa_function_type1(ctx, xr_type_new_int(X), receiver);
    if (strcmp(name, "isNull") == 0)
        return xr_type_new_function(X, NULL, 0, xr_type_new_bool(X), false);
    return NULL;
}

static XrType *xa_static_capacity_method_type(XaInferContext *ctx, AstNode *object,
                                              const char *name) {
    if (!object || object->type != AST_VARIABLE || !name || strcmp(name, "withCapacity") != 0)
        return NULL;
    const char *type_name = object->as.variable.name;
    XrVMRuntime *X = ctx->analyzer->isolate;
    if (strcmp(type_name, "Array") == 0) {
        XrType *elem = xr_type_new_unknown(X);
        return xa_function_type1(ctx, xr_type_new_int(X), xr_type_new_array(X, elem));
    }
    return NULL;
}

static XrType *xa_raw_pointer_type_namespace(XaInferContext *ctx, AstNode *object) {
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
    XrType *pointee = xr_tref_resolve_in_analyzer(ctx->analyzer, ne->type_args[0]);
    if (xa_reject_error_type_success_type(ctx->analyzer, pointee, "generic type argument",
                                          ne->class_name, object ? object->line : 0,
                                          object ? object->column : 0))
        return xr_type_new_error(NULL);
    if (!pointee)
        pointee = xr_type_new_unknown(ctx->analyzer->isolate);
    return xr_type_new_pointer(ctx->analyzer->isolate, pointee, is_mut);
}

static XrType *xa_raw_pointer_static_method_type(XaInferContext *ctx, AstNode *node,
                                                 AstNode *object, const char *name) {
    XrType *ptr_type = xa_raw_pointer_type_namespace(ctx, object);
    if (!ptr_type)
        return NULL;
    if (name && strcmp(name, "null") == 0)
        return xr_type_new_function(ctx->analyzer->isolate, NULL, 0, ptr_type, false);
    if (name && strcmp(name, "of") == 0) {
        XrLocation loc = {
            .file = ctx->file_path,
            .line = node ? node->line : (object ? object->line : 0),
            .column = node ? node->column : (object ? object->column : 0),
        };
        if (!xa_freestanding_profile_enabled(ctx->analyzer)) {
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                ptr_type->ptr_is_mut
                    ? "RawMut.of general lvalue address-taking is not enabled; the current "
                      "surface only exposes verified freestanding static data"
                    : "RawPtr.of general lvalue address-taking is not enabled; the current "
                      "surface only exposes verified freestanding static const data",
                &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
        XrType *pointee = ptr_type->container.element_type;
        if (!pointee)
            pointee = xr_type_new_unknown(ctx->analyzer->isolate);
        XrType *params[1] = {pointee};
        return xr_type_new_function(ctx->analyzer->isolate, params, 1, ptr_type, false);
    }
    return xr_type_new_unknown(ctx->analyzer->isolate);
}

static void add_index_type_error(XaInferContext *ctx, AstNode *node, XrType *index_type,
                                 XrType *expected_type) {
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    snprintf(msg, sizeof(msg), "Index type '%s' is not assignable to expected type '%s'",
             xr_type_to_string(index_type), xr_type_to_string(expected_type));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

XR_FUNC bool xa_type_contains_float(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type))
        return false;
    if (XR_TYPE_IS_FLOAT(type))
        return true;
    if (XR_TYPE_IS_UNION(type)) {
        for (int i = 0; i < type->union_type.member_count; i++) {
            if (xa_type_contains_float(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

XR_FUNC void xa_report_float_modulo_error(XaInferContext *ctx, AstNode *node, XrType *left,
                                          XrType *right) {
    if (!ctx || !ctx->analyzer || !node)
        return;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    snprintf(msg, sizeof(msg), "modulo operator '%%' requires integer operands, got '%s' and '%s'",
             xr_type_to_string(left), xr_type_to_string(right));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static bool xa_type_is_plain_bool(XrType *type) {
    return type && XR_TYPE_IS_BOOL(type) && !type->is_nullable;
}

static bool xa_type_is_nullable_non_bool(XrType *type) {
    if (!type || !type->is_nullable || XR_TYPE_IS_UNKNOWN(type))
        return false;
    XrType *base = xr_type_non_nullable(NULL, type);
    return base && !XR_TYPE_IS_BOOL(base);
}

static bool xa_class_info_same_identity(struct XrClassInfo *a, struct XrClassInfo *b) {
    if (!a || !b)
        return false;
    if (a == b)
        return true;
    return a->scope && b->scope && a->scope == b->scope;
}

static bool xa_class_info_is_subclass_of(struct XrClassInfo *sub, struct XrClassInfo *base) {
    for (struct XrClassInfo *c = sub; c; c = c->base) {
        if (xa_class_info_same_identity(c, base))
            return true;
    }
    return false;
}

XR_FUNC void xa_check_member_visibility(XaInferContext *ctx, AstNode *node, XaSymbol *member,
                                        struct XrClassInfo *owner) {
    if (!ctx || !ctx->analyzer || !node || !member)
        return;
    if (!member->is_private && !member->is_protected)
        return;

    struct XrClassInfo *access = ctx->current_class_info;
    bool ok = false;
    if (access) {
        if (member->is_private)
            ok = xa_class_info_same_identity(access, owner);
        else
            ok = (owner != NULL && xa_class_info_is_subclass_of(access, owner));
    }
    if (ok)
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    const char *vis = member->is_private ? "private" : "protected";
    snprintf(msg, sizeof(msg), "cannot access %s member '%s' of '%s' from here", vis,
             member->name ? member->name : "?", (owner && owner->name) ? owner->name : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_VISIBILITY, msg,
                               &loc);
}

XR_FUNC void xa_check_constructor_visibility(XaInferContext *ctx, AstNode *node,
                                             struct XrClassInfo *owner) {
    if (!ctx || !ctx->analyzer || !node || !owner)
        return;
    XaSymbol *ctor = xa_class_info_lookup_member(owner, XR_KEYWORD_CONSTRUCTOR);
    if (!ctor || ctor->kind != XA_SYM_METHOD)
        return;
    xa_check_member_visibility(ctx, node, ctor, owner);
}

XR_FUNC void xa_check_condition_type(XaInferContext *ctx, AstNode *node, XrType *cond_type) {
    if (!ctx || !ctx->analyzer || !node || !cond_type || XR_TYPE_IS_UNKNOWN(cond_type))
        return;
    if (xa_type_is_plain_bool(cond_type))
        return;
    if (xa_type_is_nullable_non_bool(cond_type))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[384];
    if (cond_type->is_nullable && XR_TYPE_IS_BOOL(cond_type)) {
        snprintf(msg, sizeof(msg),
                 "a bare 'bool?' value cannot be used as a condition; use 'flag == true', "
                 "'flag != null', or 'flag ?? false'");
    } else if (XR_TYPE_IS_NULL(cond_type)) {
        snprintf(msg, sizeof(msg), "'null' cannot be used as a condition");
    } else {
        snprintf(msg, sizeof(msg),
                 "condition requires 'bool' or nullable presence (T?), got '%s'; use an explicit "
                 "comparison",
                 xr_type_to_string(cond_type));
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONDITION_TYPE, msg,
                               &loc);
}

XR_FUNC void xa_check_logical_operand_type(XaInferContext *ctx, AstNode *node, XrType *type) {
    if (!ctx || !ctx->analyzer || !node || !type || XR_TYPE_IS_UNKNOWN(type))
        return;
    if (xa_type_is_plain_bool(type))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[384];
    if (type->is_nullable) {
        snprintf(msg, sizeof(msg),
                 "logical operator operand must be 'bool', not nullable '%s'; compare explicitly "
                 "before combining",
                 xr_type_to_string(type));
    } else {
        snprintf(msg, sizeof(msg),
                 "logical operator operand must be 'bool', got '%s'; use an explicit comparison",
                 xr_type_to_string(type));
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONDITION_TYPE, msg,
                               &loc);
}

/* ============================================================================
 * Pass 2: Expression Visitors
 * ============================================================================
 * Type inference for all expression kinds: literals, variables, operators,
 * function calls, member access, containers, etc.
 * ========================================================================== */

// Forward declarations for visitors defined later in this file. The
// canonical decls also live in xanalyzer_visitor_internal.h.
// xa_visit_match_expr lives in xanalyzer_visitor_pattern.c and
// xa_visit_call lives in xanalyzer_visitor_call.c.
XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node);
XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node);

XaSymbol *xa_lookup_visible_symbol(XaInferContext *ctx, const char *name) {
    if (!ctx || !ctx->analyzer || !name)
        return NULL;

    XaScope *scope = ctx->analyzer->current_scope;
    while (scope) {
        XaSymbol *sym = xa_scope_lookup_local(scope, name);
        if (sym && sym->id != ctx->initializing_symbol_id)
            return sym;
        scope = scope->parent;
    }

    XaScope *global = ctx->analyzer->global_scope;
    if (global) {
        XaSymbol *sym = xa_scope_lookup_local(global, name);
        if (sym && sym->id != ctx->initializing_symbol_id)
            return sym;
    }
    return NULL;
}

static XaScope *xa_current_function_scope(XaInferContext *ctx) {
    if (!ctx || !ctx->analyzer)
        return NULL;
    for (XaScope *scope = ctx->analyzer->current_scope; scope; scope = scope->parent) {
        if (scope->kind == XA_SCOPE_FUNCTION)
            return scope;
    }
    return NULL;
}

static bool xa_symbol_is_outer_function_capture(XaInferContext *ctx, XaSymbol *sym) {
    if (!ctx || !sym || !sym->scope)
        return false;
    if (sym->kind != XA_SYM_VARIABLE && sym->kind != XA_SYM_PARAMETER)
        return false;
    if (sym->is_builtin || sym->is_imported || sym->scope->kind == XA_SCOPE_GLOBAL)
        return false;

    XaScope *current_fn = xa_current_function_scope(ctx);
    if (!current_fn)
        return false;
    return !xa_scope_is_descendant(sym->scope, current_fn);
}

static void xa_check_span_view_closure_capture(XaInferContext *ctx, AstNode *node, XaSymbol *sym,
                                               XrType *type) {
    if (!xa_symbol_is_outer_function_capture(ctx, sym) || !xa_type_contains_span_view(type))
        return;
    xa_check_span_value_escape(ctx, node, type, "capture Slice view in closure");
}

// Check if an AST node is a typeOf() call, return the argument variable name
const char *get_typeof_arg_name(AstNode *node) {
    if (!node || node->type != AST_CALL_EXPR)
        return NULL;
    CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_VARIABLE)
        return NULL;
    if (strcmp(call->callee->as.variable.name, "typeOf") != 0)
        return NULL;
    if (call->arg_count != 1 || !call->arguments[0])
        return NULL;
    if (call->arguments[0]->type != AST_VARIABLE)
        return NULL;
    return call->arguments[0]->as.variable.name;
}

static bool xa_undefined_variable_already_reported(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return false;
    for (XaDiagnostic *diag = ctx->analyzer->diagnostics; diag; diag = diag->next) {
        if (diag->code != XR_ERR_ANALYZE_UNDEFINED_VAR ||
            diag->location.line != (uint32_t) node->line ||
            diag->location.column != (uint32_t) node->column)
            continue;
        if ((!diag->location.file && !ctx->file_path) ||
            (diag->location.file && ctx->file_path &&
             strcmp(diag->location.file, ctx->file_path) == 0))
            return true;
    }
    return false;
}

XrType *xa_visit_variable(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_error(NULL);

    const char *name = node->as.variable.name;
    XaSymbol *sym = node->as.variable.symbol_id ? xa_scope_lookup_by_id(ctx->analyzer->global_scope,
                                                                        node->as.variable.symbol_id)
                                                : NULL;
    if (!sym)
        sym = xa_lookup_visible_symbol(ctx, name);

    if (!sym) {
        /* Prelude type names used as constructors (e.g. Atomic(0)) are not
         * declared as variables but are valid call targets. Suppress the
         * undeclared-variable error; the call visitor infers the return type. */
        XrVMRuntime *X = ctx->analyzer->isolate;
        if (X) {
            const XrPreludeSymbols *symbols = xr_prelude_get_symbols(X);
            if (symbols && xr_prelude_lookup_type(symbols, name, strlen(name)))
                return xr_type_new_unknown(NULL);
        }

        // Undeclared variable — detect common cross-language mistakes
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        if (strcmp(name, "True") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'True'. Use lowercase 'true' in Xray");
        } else if (strcmp(name, "False") == 0) {
            snprintf(msg, sizeof(msg),
                     "Undeclared variable 'False'. Use lowercase 'false' in Xray");
        } else if (strcmp(name, "None") == 0) {
            snprintf(msg, sizeof(msg),
                     "Undeclared variable 'None'. Use 'null' instead of 'None' in Xray");
        } else if (strcmp(name, "nil") == 0) {
            snprintf(msg, sizeof(msg),
                     "Undeclared variable 'nil'. Use 'null' instead of 'nil' in Xray");
        } else if (strcmp(name, "undefined") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'undefined'. Use 'null' in Xray");
        } else if (strcmp(name, "self") == 0) {
            snprintf(
                msg, sizeof(msg),
                "Undeclared variable 'self'. Use 'this' to refer to the current instance in Xray");
        } else {
            snprintf(msg, sizeof(msg), "Undeclared variable '%s'", name);
        }
        if (!xa_undefined_variable_already_reported(ctx, node))
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_UNDEFINED_VAR, msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    /* Write back resolved symbol ID for Xi lowering (Braun SSA key). */
    node->as.variable.symbol_id = sym->id;
    xa_parallel_capture_check(ctx, node, sym, false);

    if (sym->kind == XA_SYM_ENUM) {
        XaSymbolLinks *enum_links = xa_analyzer_get_links(ctx->analyzer, sym);
        XrType *enum_type = enum_links && enum_links->type ? enum_links->type : NULL;
        if (!enum_type || !XR_TYPE_IS_ENUM(enum_type))
            enum_type = sym->name ? xr_type_new_enum(ctx->analyzer->isolate, sym->name)
                                  : xr_type_new_unknown(NULL);
        xa_analyzer_set_node_type(ctx->analyzer, node, enum_type);
        return enum_type;
    }

    // Record dependency: current function depends on this symbol
    if (ctx->current_function && ctx->analyzer->incremental) {
        XaIncrementalCtx *incr = (XaIncrementalCtx *) ctx->analyzer->incremental;
        xa_dep_add(incr, ctx->current_function->id, sym->id, XA_DEP_REFERENCE);
    }

    // Record reference location for Find References
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        uint32_t end_col = node->column + (name ? strlen(name) : 0);
        xa_symbol_add_ref(links, node->line, node->column, end_col, false);
    }

    // Definite assignment check: warn if variable used before initialization.
    // Tagged with XR_ERR_ANALYZE_USED_BEFORE_ASSIGN so the closure-body
    // visitor can selectively discard the false positives this check produces
    // for variables captured from enclosing scopes (the closure runs lazily,
    // so the variable may be assigned before the closure is called even if
    // the assignment textually follows the closure literal).
    if (links && !links->is_definitely_assigned && !sym->is_builtin &&
        (sym->kind == XA_SYM_VARIABLE ||
         (sym->kind == XA_SYM_PARAMETER && sym->passing_mode == XR_PARAM_OUT))) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        if (sym->kind == XA_SYM_PARAMETER) {
            snprintf(msg, sizeof(msg), "out parameter '%s' is read before being assigned", name);
        } else {
            snprintf(msg, sizeof(msg), "Variable '%s' is used before being assigned", name);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_ANALYZE_USED_BEFORE_ASSIGN, msg, &loc);
    }

    // Use-after-move check: moved variables cannot be accessed. A direct
    // `move x` source is allowed to read x when this exact move expression is
    // being re-inferred after an earlier visit already marked it moved.
    bool is_current_move_source = ctx->current_move_source_node == node &&
                                  ctx->current_move_source_symbol_id == sym->id &&
                                  ctx->current_move_source_allows_stale_mark;
    if (links && links->move_state == XA_MOVE_MOVED && !is_current_move_source) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "Variable '%s' used after move (moved at line %u)", name,
                 links->moved_line);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
    }

    // Get declared type
    XrType *declared_type = xa_analyzer_get_type(ctx->analyzer, sym);

    // Apply flow-based type narrowing for storage that can change type
    // along control flow: locals AND parameters. Functions, classes,
    // modules, type aliases never narrow because their type is the
    // declared identity, not a value.
    if (ctx->flow && ctx->flow->current_flow && declared_type &&
        (sym->kind == XA_SYM_VARIABLE || sym->kind == XA_SYM_PARAMETER)) {
        XrType *narrowed = xa_flow_get_type_of_reference(ctx->flow, name, declared_type,
                                                         ctx->flow->current_flow, ctx->cache);
        // Never means unreachable flow path — fall back to declared type
        if (narrowed && narrowed != declared_type && !XR_TYPE_IS_NEVER(narrowed)) {
            xa_check_span_view_closure_capture(ctx, node, sym, narrowed);
            // Side table is the canonical type store.
            xa_analyzer_set_node_type(ctx->analyzer, node, narrowed);
            return narrowed;
        }
    }

    xa_check_span_view_closure_capture(ctx, node, sym, declared_type);

    // Store the declared type in the analyzer side table for codegen.
    xa_analyzer_set_node_type(ctx->analyzer, node, declared_type);
    return declared_type;
}

// Compute arithmetic result for a single (left, right) pair of scalar types.
// Handles ADD/SUB/MUL/DIV/MOD with the same rules used before union
// distribution; returns NULL when the pair is incompatible so the caller
// can decide whether the overall result must collapse to unknown.
static XrType *binary_arith_pair(int op, XrType *left, XrType *right) {
    if (!left || !right)
        return NULL;
    if (XR_TYPE_IS_UNKNOWN(left) || XR_TYPE_IS_UNKNOWN(right))
        return NULL;

    if (op == AST_BINARY_ADD) {
        // string + string => string; string + (int|float|bool) => string
        // (dynamic concat handled by OP_ADD)
        if (XR_TYPE_IS_STRING(left) || XR_TYPE_IS_STRING(right))
            return xr_type_new_string(NULL);
    }
    if (op == AST_BINARY_MUL) {
        if ((XR_TYPE_IS_STRING(left) && XR_TYPE_IS_INT(right)) ||
            (XR_TYPE_IS_INT(left) && XR_TYPE_IS_STRING(right)))
            return xr_type_new_string(NULL);
    }

    if (op == AST_BINARY_MOD && (XR_TYPE_IS_FLOAT(left) || XR_TYPE_IS_FLOAT(right)))
        return NULL;

    if (XR_TYPE_IS_FLOAT(left) || XR_TYPE_IS_FLOAT(right)) {
        if ((XR_TYPE_IS_FLOAT(left) || XR_TYPE_IS_INT(left)) &&
            (XR_TYPE_IS_FLOAT(right) || XR_TYPE_IS_INT(right))) {
            // Preserve float32 width when no operand forces float64, so the
            // emitter selects single-precision opcodes that match AOT's
            // per-operand float narrowing. Like C, an int operand promotes to
            // the float operand's type (float32 * int -> float32), while any
            // float64 operand widens the result to float64. Scoped to the ops
            // that have single-precision opcodes (+, -, *, /).
            if (op == AST_BINARY_ADD || op == AST_BINARY_SUB || op == AST_BINARY_MUL ||
                op == AST_BINARY_DIV) {
                bool left_f32 = XR_TYPE_IS_FLOAT(left) && left->native_width == XR_NATIVE_F32;
                bool right_f32 = XR_TYPE_IS_FLOAT(right) && right->native_width == XR_NATIVE_F32;
                bool left_f64 = XR_TYPE_IS_FLOAT(left) && left->native_width != XR_NATIVE_F32;
                bool right_f64 = XR_TYPE_IS_FLOAT(right) && right->native_width != XR_NATIVE_F32;
                if ((left_f32 || right_f32) && !left_f64 && !right_f64)
                    return xr_type_new_float_width(NULL, XR_NATIVE_F32);
            }
            return xr_type_new_float(NULL);
        }
        return NULL;
    }
    if (XR_TYPE_IS_INT(left) && XR_TYPE_IS_INT(right)) {
        uint8_t lw = left->native_width;
        uint8_t rw = right->native_width;
        if (lw != 0 && (rw == 0 || rw == lw))
            return xr_type_new_int_width(NULL, lw);
        if (rw != 0 && lw == 0)
            return xr_type_new_int_width(NULL, rw);
        return xr_type_new_int(NULL);
    }

    // Generic body: preserve type parameter through arithmetic so
    // `fn add_one<T>(x: T): T { return x + 1 }` type-checks before
    // monomorphisation.
    if (left->kind == XR_KIND_TYPE_PARAM)
        return left;
    if (right->kind == XR_KIND_TYPE_PARAM)
        return right;

    return NULL;
}

static XrType *binary_int_result_pair(XrType *left, XrType *right, bool shift) {
    if (!left || !right || !XR_TYPE_IS_INT(left) || !XR_TYPE_IS_INT(right))
        return NULL;
    if (shift) {
        return left->native_width != 0 ? xr_type_new_int_width(NULL, left->native_width)
                                       : xr_type_new_int(NULL);
    }
    uint8_t lw = left->native_width;
    uint8_t rw = right->native_width;
    if (lw != 0 && (rw == 0 || rw == lw))
        return xr_type_new_int_width(NULL, lw);
    if (rw != 0 && lw == 0)
        return xr_type_new_int_width(NULL, rw);
    return xr_type_new_int(NULL);
}

// Distribute a binary arithmetic op over union members so e.g.
// (int|string) + (int|string) infers int|string instead of unknown.
// Falls back to unknown if any member combination is incompatible
// (caller already validated the operand-set against the operator).
static XrType *binary_arith_distribute(XaInferContext *ctx, int op, XrType *left, XrType *right) {
    int lc = XR_TYPE_IS_UNION(left) ? left->union_type.member_count : 1;
    int rc = XR_TYPE_IS_UNION(right) ? right->union_type.member_count : 1;
    XrType *single_l = XR_TYPE_IS_UNION(left) ? NULL : left;
    XrType *single_r = XR_TYPE_IS_UNION(right) ? NULL : right;

    XrType *acc = NULL;
    XrVMRuntime *X = ctx && ctx->analyzer ? ctx->analyzer->isolate : NULL;
    for (int i = 0; i < lc; i++) {
        XrType *li = single_l ? single_l : left->union_type.members[i];
        for (int j = 0; j < rc; j++) {
            XrType *rj = single_r ? single_r : right->union_type.members[j];
            XrType *r = binary_arith_pair(op, li, rj);
            if (!r) {
                // Skip incompatible pairs (e.g. string + int when both
                // sides are int|string) — runtime narrowing eliminates
                // these at the VM level.
                continue;
            }
            acc = acc ? xr_type_union(X, acc, r) : r;
        }
    }
    return acc ? acc : xr_type_new_unknown(NULL);
}

static const char *binary_operator_spelling(int op) {
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
        case AST_BINARY_BAND:
            return "&";
        case AST_BINARY_BOR:
            return "|";
        case AST_BINARY_BXOR:
            return "^";
        case AST_BINARY_LSHIFT:
            return "<<";
        case AST_BINARY_RSHIFT:
            return ">>";
        default:
            return "?";
    }
}

static void xa_report_binary_operator_type_error(XaInferContext *ctx, AstNode *node, int op,
                                                 XrType *left, XrType *right) {
    if (!ctx || !ctx->analyzer || !node || !left || !right)
        return;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    snprintf(msg, sizeof(msg), "operator '%s' is not defined for '%s' and '%s'",
             binary_operator_spelling(op), xr_type_to_string(left), xr_type_to_string(right));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static bool xa_type_may_use_dynamic_operator(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type))
        return true;
    if (type->kind == XR_KIND_TYPE_PARAM)
        return true;
    if (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE ||
        type->kind == XR_KIND_INTERFACE)
        return true;
    if (XR_TYPE_IS_UNION(type)) {
        for (int i = 0; i < type->union_type.member_count; i++) {
            if (xa_type_may_use_dynamic_operator(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool xa_binary_operator_should_report_static_error(XrType *left, XrType *right) {
    return !xa_type_may_use_dynamic_operator(left) && !xa_type_may_use_dynamic_operator(right);
}

static bool xa_type_allows_null_comparison(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type))
        return true;
    return XR_TYPE_IS_NULL(type) || type->is_nullable || xr_type_intrinsically_includes_null(type);
}

static void xa_check_null_comparison(XaInferContext *ctx, AstNode *node, XrType *left,
                                     XrType *right) {
    bool left_null = left && XR_TYPE_IS_NULL(left);
    bool right_null = right && XR_TYPE_IS_NULL(right);
    if (left_null == right_null)
        return;

    XrType *other = left_null ? right : left;
    if (xa_type_allows_null_comparison(other))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    snprintf(msg, sizeof(msg), "Comparison: cannot compare non-nullable type '%s' with null",
             xr_type_to_string(other));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static bool xa_node_is_typeof_call(AstNode *node) {
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    return call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
           strcmp(call->callee->as.variable.name, "typeOf") == 0;
}

static bool xa_node_is_string_literal(AstNode *node) {
    return node && node->type == AST_LITERAL_STRING;
}

static void xa_check_removed_typeof_string_compare(XaInferContext *ctx, AstNode *node) {
    AstNode *left = node->as.binary.left;
    AstNode *right = node->as.binary.right;
    if (!((xa_node_is_typeof_call(left) && xa_node_is_string_literal(right)) ||
          (xa_node_is_typeof_call(right) && xa_node_is_string_literal(left))))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                               "typeOf() returns TypeId; compare with Type.xxx or use typeName() "
                               "for debug strings",
                               &loc);
}

XrType *xa_visit_binary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_error(NULL);

    XrType *saved_expected = ctx->expected_type;
    ctx->expected_type = NULL;
    XrType *left = xa_visit_infer_expr(ctx, node->as.binary.left);
    XrType *right;
    if ((node->type == AST_BINARY_AND || node->type == AST_BINARY_OR) && ctx->flow &&
        ctx->flow->current_flow) {
        // Short-circuit narrowing: the right operand only runs once the left's
        // truthiness is known. `a && b` evaluates b assuming a is true; `a || b`
        // evaluates b assuming a is false. This lets idioms like
        // `x != null && x.field` and `x == null || x.field` narrow x for the
        // right operand (matching how `if` narrows its body).
        XaFlowNode *saved_flow = ctx->flow->current_flow;
        ctx->flow->current_flow =
            xa_flow_create_condition(ctx->flow, node->as.binary.left, node->type == AST_BINARY_AND);
        right = xa_visit_infer_expr(ctx, node->as.binary.right);
        ctx->flow->current_flow = saved_flow;
    } else {
        right = xa_visit_infer_expr(ctx, node->as.binary.right);
    }
    ctx->expected_type = saved_expected;

    if (XR_TYPE_IS_ERROR(left))
        return left;
    if (XR_TYPE_IS_ERROR(right))
        return right;

    if (xa_freestanding_profile_enabled(ctx->analyzer) && node->type == AST_BINARY_ADD &&
        (XR_TYPE_IS_STRING(left) || XR_TYPE_IS_STRING(right))) {
        xa_freestanding_report_unavailable(
            ctx, node, "string concatenation",
            "use static string literals or write into an explicit user buffer");
    }

    // Deterministic result types: language rules independent of operand types
    switch (node->type) {
        // Comparison → always bool
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
            xa_check_null_comparison(ctx, node, left, right);
            xa_check_removed_typeof_string_compare(ctx, node);
            return xr_type_new_bool(NULL);
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
            return xr_type_new_bool(NULL);
        // Logical → always bool
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            xa_check_logical_operand_type(ctx, node->as.binary.left, left);
            xa_check_logical_operand_type(ctx, node->as.binary.right, right);
            return xr_type_new_bool(NULL);
        // Bitwise → always int
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT: {
            XrType *r = binary_int_result_pair(
                left, right, node->type == AST_BINARY_LSHIFT || node->type == AST_BINARY_RSHIFT);
            if (r)
                return r;
            if (xa_binary_operator_should_report_static_error(left, right)) {
                xa_report_binary_operator_type_error(ctx, node, node->type, left, right);
                return xr_type_new_error(ctx->analyzer->isolate);
            }
            return xr_type_new_unknown(NULL);
        }
        default:
            break;
    }

    // Arithmetic: result depends on operand types
    if (XR_TYPE_IS_UNKNOWN(left) || XR_TYPE_IS_UNKNOWN(right)) {
        return xr_type_new_unknown(NULL);
    }

    if (node->type == AST_BINARY_MOD &&
        (xa_type_contains_float(left) || xa_type_contains_float(right))) {
        xa_report_float_modulo_error(ctx, node, left, right);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    switch (node->type) {
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD: {
            XrType *result = binary_arith_distribute(ctx, node->type, left, right);
            if (result && XR_TYPE_IS_UNKNOWN(result) &&
                xa_binary_operator_should_report_static_error(left, right)) {
                xa_report_binary_operator_type_error(ctx, node, node->type, left, right);
                return xr_type_new_error(ctx->analyzer->isolate);
            }
            return result;
        }
        default:
            return xr_type_new_unknown(NULL);
    }
}

XrType *xa_visit_unary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *saved_expected = ctx->expected_type;
    ctx->expected_type = NULL;
    XrType *operand = xa_visit_infer_expr(ctx, node->as.unary.operand);
    ctx->expected_type = saved_expected;

    switch (node->type) {
        case AST_UNARY_NEG:
            return operand;  // -x has same type as x
        case AST_UNARY_NOT:
            xa_check_logical_operand_type(ctx, node, operand);
            return xr_type_new_bool(NULL);
        case AST_UNARY_BNOT:
            return XR_TYPE_IS_INT(operand) ? operand : xr_type_new_int(NULL);
        default:
            return xr_type_new_unknown(NULL);
    }
}

/* ----------------------------------------------------------------------------
 * Member Access Type Inference
 * -------------------------------------------------------------------------- */
// Under strict null checks (default on), accessing a member/index of — or
// calling — a value whose static type is still nullable is a compile error:
// the operation would panic at runtime if the value is null. The programmer
// must narrow first (an `if x != null` check, optional-chaining `?.`, or the
// `!` non-null assertion), all of which strip the nullable flag before we get
// here. Returns true if an error was reported.
static bool xa_check_nullable_access(XaInferContext *ctx, AstNode *node, XrType *recv_type,
                                     const char *access_desc) {
    if (!ctx || !ctx->analyzer || !ctx->analyzer->strict_null_checks)
        return false;
    if (!recv_type || XR_TYPE_IS_UNKNOWN(recv_type) || !recv_type->is_nullable)
        return false;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "%s on a possibly-null value: narrow it first with `if x != null`, use `?.`, "
             "or assert non-null with `!`",
             access_desc);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_POSSIBLY_NULL, msg,
                               &loc);
    return true;
}

XrType *xa_visit_member_access(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_error(NULL);

    MemberAccessNode *ma = &node->as.member_access;

    if (ma->object && ma->object->type == AST_VARIABLE && ma->object->as.variable.name &&
        strcmp(ma->object->as.variable.name, "Type") == 0) {
        if (xr_type_from_name(ma->name) >= 0)
            return xr_type_new_int(ctx->analyzer->isolate);
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown TypeId constant 'Type.%s'", ma->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    XrType *raw_pointer_static_fn =
        xa_raw_pointer_static_method_type(ctx, node, ma->object, ma->name);
    if (raw_pointer_static_fn)
        return raw_pointer_static_fn;

    XrType *freestanding_static_member =
        xa_freestanding_reject_owned_static_member(ctx, ma->object, ma->name, node);
    if (freestanding_static_member)
        return freestanding_static_member;

    XrType *expected_enum_member = xa_try_expected_enum_member_access(ctx, node, ma);
    if (expected_enum_member)
        return expected_enum_member;

    XrType *obj_type = xa_visit_infer_expr(ctx, ma->object);

    // A diagnosed receiver failure is recovery poison, not an unresolved type.
    // Propagate it without emitting a secondary member-access diagnostic.
    if (XR_TYPE_IS_ERROR(obj_type))
        return obj_type;

    // Reject `.member` on a possibly-null receiver (strict null checks).
    xa_check_nullable_access(ctx, node, obj_type, "member access");

    if (xa_freestanding_reject_string_member(ctx, node, obj_type, ma->name))
        return xr_type_new_error(ctx->analyzer->isolate);

    XrType *static_capacity_fn = xa_static_capacity_method_type(ctx, ma->object, ma->name);
    if (static_capacity_fn)
        return static_capacity_fn;

    // Check module member access before the unknown-type early return (e.g., net.dial)
    if (XR_TYPE_IS_UNKNOWN(obj_type) && ma->object->type == AST_VARIABLE) {
        const char *var_name = ma->object->as.variable.name;
        XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var_name);
        if (sym && sym->kind == XA_SYM_MODULE) {
            XaSymbolLinks *sym_links = xa_analyzer_get_links(ctx->analyzer, sym);
            const char *mod_name =
                (sym_links && sym_links->module_name) ? sym_links->module_name : var_name;
            bool is_quoted = (mod_name[0] == '.' || mod_name[0] == '/');
            XrHashMap *exports = resolve_graph_export_symbols(ctx->analyzer, mod_name, is_quoted);
            if (exports) {
                XaSymbol *member_sym = (XaSymbol *) xr_hashmap_get(exports, ma->name);
                if (member_sym) {
                    XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member_sym);
                    XrType *member_type = member_links ? member_links->type : NULL;
                    if (member_type) {
                        record_selection(ctx, node, XA_SEL_MODULE_EXPORT, obj_type, member_sym, -1,
                                         member_type, false);
                        return member_type;
                    }
                    if (member_sym->kind == XA_SYM_CLASS && member_links &&
                        member_links->class_info) {
                        XrType *class_type =
                            xr_type_new_class(ctx->analyzer->isolate, member_sym->name);
                        if (class_type)
                            class_type->instance.class_ref = member_links->class_info;
                        record_selection(ctx, node, XA_SEL_MODULE_EXPORT, obj_type, member_sym, -1,
                                         class_type, false);
                        return class_type ? class_type : xr_type_new_unknown(NULL);
                    }
                }
            }

            const XaBuiltinModule *mod = xa_builtin_get_module_info(mod_name);
            if (mod) {
                // Look up member (function or constant) in module type table
                const char *sig = xa_builtin_get_module_func_signature(mod_name, ma->name);
                if (sig) {
                    if (xa_freestanding_profile_enabled(ctx->analyzer) &&
                        !xa_freestanding_stdlib_member_allowed(mod_name, ma->name)) {
                        char feature[192];
                        snprintf(feature, sizeof(feature), "%s.%s", mod_name, ma->name);
                        xa_freestanding_report_unavailable(
                            ctx, node, feature,
                            xa_freestanding_stdlib_member_reject_suggestion(mod_name));
                    }
                    XrType *mod_result = NULL;
                    // Constant property: signature is ": type" (no parens)
                    if (sig[0] == ':') {
                        const char *type_str = sig + 1;
                        while (*type_str == ' ')
                            type_str++;
                        mod_result = xa_builtin_parse_type_string(ctx->analyzer->isolate, type_str);
                    } else {
                        // Function: parse complete signature (params + return type)
                        mod_result = xa_builtin_parse_full_signature(ctx->analyzer->isolate, sig);
                    }
                    if (mod_result) {
                        record_selection(ctx, node, XA_SEL_MODULE_EXPORT, obj_type, sym, -1,
                                         mod_result, false);
                    }
                    return mod_result;
                }
                xa_report_unknown_stdlib_member(ctx, node, mod_name, ma->name);
                return xr_type_new_error(ctx->analyzer->isolate);
            }

            /* User module namespaces are resolved above from graph exports. */
        }
    }

    /* Tuple field access via `.N` -- the parser already encoded the
     * digit run as the member name (see xparse_decl.c). We accept only
     * digit-only names on tuple receivers; anything else is a hard
     * error. The numeric value is parsed in C and bounds-checked
     * against the tuple's static arity. */
    if (XR_TYPE_IS_TUPLE(obj_type)) {
        const char *nm = ma->name;
        bool digits_only = (nm && *nm);
        for (const char *p = nm; *p && digits_only; p++) {
            if (*p < '0' || *p > '9')
                digits_only = false;
        }
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        if (!digits_only) {
            char msg[160];
            snprintf(msg, sizeof(msg), "tuple has no named field '%s'; use .N (zero-based) instead",
                     nm ? nm : "");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TUPLE_FIELD_NAME, msg, &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
        long idx = strtol(nm, NULL, 10);
        int arity = obj_type->tuple.element_count;
        if (idx < 0 || idx >= arity) {
            char msg[128];
            snprintf(msg, sizeof(msg), "tuple field index %ld out of range (arity %d)", idx, arity);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TUPLE_FIELD_RANGE, msg, &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
        return obj_type->tuple.element_types[(int) idx];
    }

    // Enum namespace/value access. `Color.Red` reads a variant from the
    // enum namespace. `Color.staticFn` resolves a static method. Any other
    // enum-typed receiver is an enum value and can resolve instance methods.
    if (obj_type->kind == XR_KIND_ENUM && obj_type->enum_type.enum_name) {
        XaSymbol *enum_sym = NULL;
        if (ma->object && ma->object->type == AST_VARIABLE &&
            ma->object->as.variable.symbol_id != 0) {
            XaSymbol *by_id = xa_scope_lookup_by_id(ctx->analyzer->global_scope,
                                                    ma->object->as.variable.symbol_id);
            if (by_id && by_id->kind == XA_SYM_ENUM && by_id->name &&
                strcmp(by_id->name, obj_type->enum_type.enum_name) == 0)
                enum_sym = by_id;
        }
        if (!enum_sym)
            enum_sym = xa_scope_lookup(ctx->analyzer->current_scope, obj_type->enum_type.enum_name);
        if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
            XaSymbolLinks *el = xa_analyzer_get_links(ctx->analyzer, enum_sym);
            if (el) {
                bool is_namespace =
                    member_object_is_enum_namespace(ctx, ma->object, obj_type->enum_type.enum_name);
                if (is_namespace) {
                    XaEnumInfo *enum_info = el->enum_info;
                    for (uint32_t i = 0; enum_info && i < enum_info->variant_count; i++) {
                        if (enum_info->variants[i].name &&
                            strcmp(enum_info->variants[i].name, ma->name) == 0) {
                            if (enum_info->variants[i].payload_count > 0 &&
                                !ctx->allow_payload_enum_ctor_value) {
                                XrLocation loc = {
                                    .file = ctx->file_path,
                                    .line = node->line,
                                    .column = node->column,
                                };
                                char msg[192];
                                snprintf(msg, sizeof(msg),
                                         "payload enum variant '%s.%s' is a constructor; call it "
                                         "as '%s.%s(...)' instead of using it as a value",
                                         obj_type->enum_type.enum_name, ma->name,
                                         obj_type->enum_type.enum_name, ma->name);
                                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                           XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
                                return xr_type_new_error(ctx->analyzer->isolate);
                            }
                            XrType *enum_type = xr_type_new_enum(ctx->analyzer->isolate,
                                                                 obj_type->enum_type.enum_name);
                            enum_type->enum_type.layout = enum_info->layout;
                            enum_type->enum_type.layout_id =
                                enum_info->layout ? enum_info->layout->layout_id : 0;
                            record_selection(ctx, node, XA_SEL_ENUM_MEMBER, obj_type, enum_sym, i,
                                             enum_type, false);
                            return enum_type;
                        }
                    }
                    if (el->class_info) {
                        XaSymbol *member =
                            xa_class_info_lookup_static_member(el->class_info, ma->name);
                        if (member && member->kind == XA_SYM_METHOD && member->is_static) {
                            XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                            if (ml && ml->type) {
                                record_selection(ctx, node, XA_SEL_STATIC_MEMBER, obj_type, member,
                                                 -1, ml->type, false);
                                return ml->type;
                            }
                        }
                    }
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[160];
                    snprintf(msg, sizeof(msg), "enum '%s' has no member '%s'",
                             obj_type->enum_type.enum_name, ma->name ? ma->name : "");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
                    return xr_type_new_error(ctx->analyzer->isolate);
                } else if (el->class_info) {
                    XaSymbol *member =
                        xa_class_info_lookup_instance_member(el->class_info, ma->name);
                    if (member && member->kind == XA_SYM_METHOD && !member->is_static) {
                        XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                        if (ml && ml->type) {
                            record_selection(ctx, node, XA_SEL_METHOD, obj_type, member, -1,
                                             ml->type, false);
                            return ml->type;
                        }
                    }
                }
                if (!is_namespace) {
                    if (strcmp(ma->name, "name") == 0) {
                        if (xa_freestanding_profile_enabled(ctx->analyzer) &&
                            (!el->enum_info || el->enum_info->is_payload_enum)) {
                            xa_freestanding_report_unavailable(
                                ctx, node, "enum.name",
                                "payload enum name materialization still needs hosted string "
                                "helpers; use ordinal or typed match in freestanding code");
                            return xr_type_new_error(ctx->analyzer->isolate);
                        }
                        return xr_type_new_string(NULL);
                    }
                    if (strcmp(ma->name, "ordinal") == 0) {
                        return xr_type_new_int(NULL);
                    }
                    if (strcmp(ma->name, "toString") == 0) {
                        if (xa_freestanding_profile_enabled(ctx->analyzer) &&
                            (!el->enum_info || el->enum_info->is_payload_enum)) {
                            xa_freestanding_report_unavailable(
                                ctx, node, "enum.toString",
                                "payload enum string materialization still needs hosted string "
                                "helpers; use ordinal or typed match in freestanding code");
                            return xr_type_new_error(ctx->analyzer->isolate);
                        }
                        XrType *ret = xr_type_new_string(NULL);
                        return xr_type_new_function(ctx->analyzer->isolate, NULL, 0, ret, false);
                    }
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[160];
                    snprintf(msg, sizeof(msg), "enum value has no member '%s'",
                             ma->name ? ma->name : "");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
                    return xr_type_new_error(ctx->analyzer->isolate);
                }
            }
        }
        return xr_type_new_unknown(NULL);
    }

    // Class static member access: ClassName.staticMethod
    if (obj_type->kind == XR_KIND_CLASS && obj_type->instance.class_name) {
        XrClassInfo *class_info = obj_type->instance.class_ref;
        if (!class_info) {
            XaSymbol *class_sym =
                xa_scope_lookup(ctx->analyzer->current_scope, obj_type->instance.class_name);
            XaSymbolLinks *class_links = class_sym && class_sym->kind == XA_SYM_CLASS
                                             ? xa_analyzer_get_links(ctx->analyzer, class_sym)
                                             : NULL;
            class_info = class_links ? class_links->class_info : NULL;
        }
        if (class_info) {
            XaSymbol *member = xa_class_info_lookup_static_member(class_info, ma->name);
            if (member) {
                XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                if (ml && ml->type) {
                    record_selection(ctx, node, XA_SEL_STATIC_MEMBER, obj_type, member, -1,
                                     ml->type, false);
                    return ml->type;
                }
            }
        }
    }

    // Unknown preserves error recovery and IDE responsiveness after imprecise analysis.
    if (XR_TYPE_IS_UNKNOWN(obj_type)) {
        return xr_type_new_unknown(NULL);
    }

    // Union type member access: every member must declare the member with
    // compatible types. Returns the joined function/field type so callers
    // see a single coherent signature instead of `unknown`. This is what
    // makes virtual-style dispatch over `Array<Dog | Cat>` type-check.
    if (XR_TYPE_IS_UNION(obj_type)) {
        XrType *joined = NULL;
        for (int i = 0; i < obj_type->union_type.member_count; i++) {
            XrType *m = obj_type->union_type.members[i];
            if (!m)
                continue;
            XrType *member_ty = NULL;
            // Class instance: look up method/field by name in class info.
            if (XR_TYPE_IS_INSTANCE(m) && m->instance.class_name) {
                XaSymbol *cs =
                    xa_scope_lookup(ctx->analyzer->current_scope, m->instance.class_name);
                if (cs) {
                    XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, cs);
                    if (cl && cl->class_info) {
                        XaSymbol *mem =
                            xa_class_info_lookup_instance_member(cl->class_info, ma->name);
                        if (mem) {
                            XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, mem);
                            if (ml && ml->type)
                                member_ty = ml->type;
                        }
                    }
                }
            }
            if (!member_ty) {
                joined = NULL;
                break;
            }
            joined = joined ? xr_type_union(ctx->analyzer->isolate, joined, member_ty) : member_ty;
        }
        if (joined)
            return joined;
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "union type '%s' has no member '%s'",
                 xr_type_to_string(obj_type), ma->name ? ma->name : "");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    // Handle built-in properties
    if ((obj_type->kind == XR_KIND_INTERFACE || obj_type->kind == XR_KIND_INSTANCE) &&
        obj_type->instance.class_name && strcmp(obj_type->instance.class_name, "Iterator") == 0) {
        XrType *elem = (obj_type->instance.type_arg_count > 0 && obj_type->instance.type_args &&
                        obj_type->instance.type_args[0])
                           ? obj_type->instance.type_args[0]
                           : xr_type_new_unknown(ctx->analyzer->isolate);
        if (strcmp(ma->name, "hasNext") == 0)
            return xr_type_new_function(ctx->analyzer->isolate, NULL, 0, xr_type_new_bool(NULL),
                                        false);
        if (strcmp(ma->name, "next") == 0)
            return xr_type_new_function(ctx->analyzer->isolate, NULL, 0, elem, false);
        if (strcmp(ma->name, "nth") == 0) {
            XrType *params[1] = {xr_type_new_int(NULL)};
            return xr_type_new_function(ctx->analyzer->isolate, params, 1, elem, false);
        }
        if (strcmp(ma->name, "iterator") == 0)
            return xr_type_new_function(ctx->analyzer->isolate, NULL, 0, obj_type, false);
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[160];
        snprintf(msg, sizeof(msg), "Iterator has no member '%s'", ma->name ? ma->name : "");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (obj_type->kind == XR_KIND_INTERFACE && obj_type->instance.class_name) {
        XaSymbol *iface_sym = xa_analyzer_lookup_deep(ctx->analyzer, obj_type->instance.class_name);
        XaSymbolLinks *iface_links =
            iface_sym ? xa_analyzer_get_links(ctx->analyzer, iface_sym) : NULL;
        XrClassInfo *iface_info = iface_links ? iface_links->class_info : NULL;
        if (iface_info) {
            XaSymbol *member = xa_class_info_lookup_instance_member(iface_info, ma->name);
            if (member) {
                XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member);
                if (member_links && member_links->type) {
                    XrType *member_type = member_links->type;
                    int type_param_count =
                        iface_links ? xa_symbol_links_get_type_param_count(iface_links) : 0;
                    if (type_param_count > 0 && obj_type->instance.type_arg_count > 0) {
                        const char **param_names =
                            xr_malloc(sizeof(const char *) * type_param_count);
                        for (int i = 0; i < type_param_count; i++)
                            param_names[i] = xa_symbol_links_get_type_param_name(iface_links, i);
                        member_type = xr_type_substitute(ctx->analyzer->isolate, member_type,
                                                         param_names, obj_type->instance.type_args,
                                                         obj_type->instance.type_arg_count);
                        xr_free(param_names);
                    }
                    XaSelectionKind sk =
                        (member->kind == XA_SYM_METHOD) ? XA_SEL_METHOD : XA_SEL_FIELD;
                    record_selection(ctx, node, sk, obj_type, member, -1, member_type, false);
                    return member_type;
                }
            }
        }
    }

    SymbolId prop_sym = xr_builtin_symbol_from_name(ma->name);
    bool declares_legacy_named_member = false;
    if (obj_type->kind == XR_KIND_INSTANCE && obj_type->instance.class_ref) {
        declares_legacy_named_member =
            xa_class_info_lookup_instance_member(obj_type->instance.class_ref, ma->name) != NULL;
    }
    if ((prop_sym == SYMBOL_LENGTH || prop_sym == SYMBOL_SIZE || prop_sym == SYMBOL_IS_EMPTY) &&
        xa_type_has_len_query(obj_type) && !declares_legacy_named_member) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[192];
        snprintf(msg, sizeof(msg), "%s has no member '%s'; use len(value)%s",
                 xr_type_to_string(obj_type), ma->name ? ma->name : "?",
                 prop_sym == SYMBOL_IS_EMPTY ? " == 0" : "");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    if (prop_sym == SYMBOL_CAPACITY && XR_TYPE_IS_ARRAY(obj_type)) {
        return xr_type_new_int(NULL);
    }
    if (XR_TYPE_IS_SPAN(obj_type) && ma->name && ctx->unsafe_depth == 0 &&
        (strcmp(ma->name, "ptr") == 0 || strcmp(ma->name, "mutPtr") == 0)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "%s.%s() must be inside an unsafe block",
                 xa_type_is_u8_slice_type(obj_type) ? "Slice<byte>" : "Slice", ma->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
    }
    if (xr_type_is_named_class(obj_type, "Buffer") && ma->name && strcmp(ma->name, "ptr") == 0 &&
        ctx->unsafe_depth == 0) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "Buffer.ptr() must be inside an unsafe block", &loc);
    }
    if (obj_type->kind == XR_KIND_CHANNEL) {
        if (prop_sym == SYMBOL_CANCELLED)
            return xr_type_new_bool(NULL);
    }

    if (xa_contextual_view_method_without_target(ctx, obj_type, ma->name, node))
        return xr_type_new_error(ctx->analyzer->isolate);

    const XaBuiltinReceiverMethodSpec *builtin_receiver_spec =
        xa_find_builtin_receiver_method_spec(obj_type, ma->name);
    if (builtin_receiver_spec) {
        xa_report_builtin_receiver_unsafe_requirement(ctx, node, obj_type, builtin_receiver_spec);
        XrType *builtin_receiver_method =
            xa_builtin_receiver_method_type_from_spec(ctx, obj_type, builtin_receiver_spec);
        return builtin_receiver_method;
    }

    XrType *string_view_method = xa_string_view_method_type(ctx, obj_type, ma->name, node);
    if (string_view_method)
        return string_view_method;

    XrType *array_view_method = xa_array_view_method_type(ctx, obj_type, ma->name, node);
    if (array_view_method)
        return array_view_method;

    XrType *data_ptr_method = xa_array_data_ptr_method_type(ctx, obj_type, ma->name);
    if (data_ptr_method)
        return data_ptr_method;

    XrType *ptr_method = xa_pointer_method_type(ctx, obj_type, ma->name, node);
    if (ptr_method)
        return ptr_method;

    if (XR_TYPE_IS_POINTER(obj_type)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[160];
        snprintf(msg, sizeof(msg), "%s has no member '%s'",
                 obj_type->ptr_is_mut ? "RawMut" : "RawPtr", ma->name ? ma->name : "");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (XR_TYPE_IS_VIEW(obj_type)) {
        xa_report_view_member_error(ctx, node, obj_type, ma->name);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (XR_TYPE_IS_SPAN(obj_type)) {
        xa_report_span_member_error(ctx, node, obj_type, ma->name);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (obj_type->kind == XR_KIND_FIXED_ARRAY) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Fixed array has no member '%s'; use len(value) or indexed access", ma->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_ref) {
        XrClassInfo *class_info = obj_type->instance.class_ref;
        struct XrClassInfo *member_owner = NULL;
        XaSymbol *member =
            xa_class_info_lookup_instance_member_owner(class_info, ma->name, &member_owner);
        if (member) {
            xa_check_member_visibility(ctx, node, member, member_owner);
            XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member);
            if (member_links && member_links->type) {
                XrType *member_type = member_links->type;
                XaSymbol *class_sym =
                    xa_scope_lookup(ctx->analyzer->current_scope, obj_type->instance.class_name);
                XaSymbolLinks *class_links =
                    class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
                int type_param_count =
                    class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
                if (type_param_count > 0 && obj_type->instance.type_arg_count > 0) {
                    const char **param_names = xr_malloc(sizeof(const char *) * type_param_count);
                    for (int i = 0; i < type_param_count; i++)
                        param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                    member_type = xr_type_substitute(ctx->analyzer->isolate, member_type,
                                                     param_names, obj_type->instance.type_args,
                                                     obj_type->instance.type_arg_count);
                    xr_free(param_names);
                }
                XaSelectionKind sk = (member->kind == XA_SYM_METHOD) ? XA_SEL_METHOD : XA_SEL_FIELD;
                if (sk == XA_SEL_FIELD && class_info && class_info->struct_layout &&
                    class_info->struct_layout->kind == XR_AGG_LAYOUT_UNION &&
                    ctx->unsafe_depth == 0) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[192];
                    snprintf(msg, sizeof(msg),
                             "union field read '%s.%s' must be inside an `unsafe { }` block",
                             class_info->name ? class_info->name : "union",
                             ma->name ? ma->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
                }
                record_selection(ctx, node, sk, obj_type, member, -1, member_type, false);
                return member_type;
            }
        }
    }

    // Handle built-in methods (return function type for method access)
    if (xa_builtin_is_method(obj_type, ma->name)) {
        const char *sig = xa_builtin_get_member_signature(obj_type, ma->name);
        if (sig) {
            XrType *fn_type = xa_builtin_parse_full_signature(ctx->analyzer->isolate, sig);
            // Substitute generic type parameters with actual container types:
            //   Array<T>/Set<T>/Channel<T>: T -> element_type
            //   Task<T>/WorkQueue<T>/Atomic<T>: T -> instance type argument
            //   Map<K,V>: K -> key_type, V -> value_type
            if (fn_type) {
                XrType *single_type_arg = NULL;
                if ((XR_TYPE_IS_ARRAY(obj_type) || obj_type->kind == XR_KIND_SET ||
                     obj_type->kind == XR_KIND_CHANNEL) &&
                    obj_type->container.element_type) {
                    single_type_arg = obj_type->container.element_type;
                } else if ((xr_type_is_named_class(obj_type, "Task") ||
                            xr_type_is_named_class(obj_type, "WorkQueue") ||
                            xr_type_is_named_class(obj_type, "Atomic")) &&
                           obj_type->instance.type_arg_count > 0 && obj_type->instance.type_args) {
                    single_type_arg = obj_type->instance.type_args[0];
                }
                if (single_type_arg) {
                    const char *names[] = {"T"};
                    XrType *types[] = {single_type_arg};
                    fn_type = xr_type_substitute(ctx->analyzer->isolate, fn_type, names, types, 1);
                } else if (XR_TYPE_IS_MAP(obj_type)) {
                    XrType *kt = obj_type->map.key_type;
                    XrType *vt = obj_type->map.value_type;
                    if (kt && vt) {
                        const char *names[] = {"K", "V"};
                        XrType *types[] = {kt, vt};
                        fn_type =
                            xr_type_substitute(ctx->analyzer->isolate, fn_type, names, types, 2);
                    }
                }
                XrType *return_type =
                    xa_builtin_get_method_return_type(ctx->analyzer->isolate, obj_type, ma->name);
                if (return_type && (!fn_type->function.return_type ||
                                    XR_TYPE_IS_UNKNOWN(fn_type->function.return_type) ||
                                    XR_TYPE_IS_JSON(fn_type->function.return_type))) {
                    fn_type->function.return_type = return_type;
                }
                return fn_type;
            }
        }
        // Fallback: return function with unknown return type
        XrType *return_type =
            xa_builtin_get_method_return_type(ctx->analyzer->isolate, obj_type, ma->name);
        if (return_type) {
            return xr_type_new_function(ctx->analyzer->isolate, NULL, 0, return_type, false);
        }
    }

    // Built-in non-method properties (e.g. Channel.closed). The signature for properties is just `:
    // T` with no parameter list. Skip the method substitution machinery above because there are no
    // type-parameter container fields exposed as property kind today.
    {
        const char *sig = xa_builtin_get_member_signature(obj_type, ma->name);
        if (sig && sig[0] == ':') {
            const char *type_str = sig + 1;
            while (*type_str == ' ')
                type_str++;
            return xa_builtin_parse_type_string(ctx->analyzer->isolate, type_str);
        }
    }

    if (XR_TYPE_IS_STRING(obj_type)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "string has no member '%s'; use the canonical string surface or text module",
                 ma->name ? ma->name : "");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (XR_TYPE_IS_ARRAY(obj_type)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[160];
        snprintf(msg, sizeof(msg), "%s has no member '%s'",
                 xa_type_is_u8_array_type(obj_type) ? "Array<byte>" : "Array", ma->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    // Handle class instance members
    if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
        XaSymbol *class_sym =
            xa_scope_lookup(ctx->analyzer->current_scope, obj_type->instance.class_name);
        XaSymbolLinks *class_links =
            class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
        /* Namespace-imported classes have no local symbol under their bare name
         * (only the module alias is in scope), so the by-name lookup misses.
         * Fall back to the class_info the instance type itself carries so
         * cross-module instance methods/fields resolve to their real signatures
         * — required to give closure arguments their expected `(T) -> U` type
         * (otherwise a lambda passed to e.g. `ns.Box<int>().map(f)` loses its
         * return type and lowers to a value-discarding `void` body). */
        XrClassInfo *class_info = (class_links && class_links->class_info)
                                      ? class_links->class_info
                                      : obj_type->instance.class_ref;
        if (class_info) {
            struct XrClassInfo *member_owner = NULL;
            XaSymbol *member =
                xa_class_info_lookup_instance_member_owner(class_info, ma->name, &member_owner);
            if (member) {
                xa_check_member_visibility(ctx, node, member, member_owner);
                XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member);
                if (member_links && member_links->type) {
                    XrType *member_type = member_links->type;

                    // Apply type substitution for generic instances
                    int type_param_count =
                        class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
                    if (type_param_count > 0 && obj_type->instance.type_arg_count > 0) {
                        const char **param_names =
                            xr_malloc(sizeof(const char *) * type_param_count);
                        for (int i = 0; i < type_param_count; i++) {
                            param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                        }
                        member_type = xr_type_substitute(ctx->analyzer->isolate, member_type,
                                                         param_names, obj_type->instance.type_args,
                                                         obj_type->instance.type_arg_count);
                        xr_free(param_names);
                    }
                    XaSelectionKind sk =
                        (member->kind == XA_SYM_METHOD) ? XA_SEL_METHOD : XA_SEL_FIELD;
                    if (sk == XA_SEL_FIELD && class_info && class_info->struct_layout &&
                        class_info->struct_layout->kind == XR_AGG_LAYOUT_UNION &&
                        ctx->unsafe_depth == 0) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[192];
                        snprintf(msg, sizeof(msg),
                                 "union field read '%s.%s' must be inside an `unsafe { }` block",
                                 class_info->name ? class_info->name : "union",
                                 ma->name ? ma->name : "?");
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
                    }
                    record_selection(ctx, node, sk, obj_type, member, -1, member_type, false);
                    return member_type;
                }
            }
        }
    }

    // Handle methods on module handle types (e.g. SqliteDB.exec from .xrd).
    // The handle type is resolved as an instance type whose class_name
    // matches a registered handle; look up methods on that handle.
    if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
        const XaBuiltinHandle *handle =
            xa_builtin_find_handle_by_name(obj_type->instance.class_name);
        if (handle) {
            // Check handle fields first
            for (int i = 0; i < handle->field_count; i++) {
                if (strcmp(handle->fields[i].name, ma->name) == 0) {
                    return xa_builtin_parse_type_string(ctx->analyzer->isolate,
                                                        handle->fields[i].type_str);
                }
            }
            // Check handle methods
            for (int i = 0; i < handle->method_count; i++) {
                if (strcmp(handle->methods[i].name, ma->name) == 0) {
                    return xa_builtin_parse_full_signature(ctx->analyzer->isolate,
                                                           handle->methods[i].signature);
                }
            }
        }
    }

    if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
        const XaBuiltinMember *builtin_members = NULL;
        if (xa_builtin_get_members_for_type(obj_type, &builtin_members) > 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[160];
            snprintf(msg, sizeof(msg), "%s has no member '%s'", obj_type->instance.class_name,
                     ma->name ? ma->name : "");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
    }

    // Handle Json/Record field access.
    // Plain Json represents any JSON value (including null), so field access returns Json.
    if (XR_TYPE_IS_JSON(obj_type) && obj_type->object.field_count == 0) {
        // Bare Json type (e.g. function parameter) — no static field info,
        // return Json since any field access is valid at runtime.
        return xr_type_new_json(ctx->analyzer->isolate);
    }
    if (XR_TYPE_HAS_OBJECT_SHAPE(obj_type) && obj_type->object.field_count > 0) {
        int field_idx = object_shape_field_index(obj_type, ma->name);
        if (field_idx >= 0 && obj_type->object.field_types) {
            XrType *ft = obj_type->object.field_types[field_idx];
            if (!ft)
                return xr_type_new_unknown(NULL);
            XrType *result_ft =
                XR_TYPE_IS_JSON(obj_type) ? xr_type_make_nullable(ctx->analyzer->isolate, ft) : ft;
            record_selection(ctx, node, XA_SEL_FIELD, obj_type, NULL, field_idx, result_ft, false);
            return result_ft;
        }
        if (obj_type->object.is_sealed) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(msg, sizeof(msg), "类型 '%s' 没有字段 '%s'", object_shape_type_label(obj_type),
                     ma->name);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
        return xr_type_new_unknown(NULL);
    }

    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_index_get(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_error(NULL);

    IndexGetNode *ig = &node->as.index_get;
    /*
     * A borrowed view used only as the immediate operand of an index expression
     * has an unambiguous, non-escaping lifetime.  Give lowered view constructors
     * that context so the canonical `s.bytes()[i]` spelling does not require a
     * throw-away Slice<byte> binding.
     */
    bool saved_view_context = ctx->allow_view_expr_for_copy;
    if (ig->array && ig->array->type == AST_CALL_EXPR) {
        CallExprNode *call = &ig->array->as.call_expr;
        if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
            const char *name = call->callee->as.member_access.name;
            if (name && (strcmp(name, "bytes") == 0 || strcmp(name, "asSpan") == 0 ||
                         strcmp(name, "asBytes") == 0 || strcmp(name, "reinterpret") == 0))
                ctx->allow_view_expr_for_copy = true;
        }
    }
    XrType *container = xa_visit_infer_expr(ctx, ig->array);
    ctx->allow_view_expr_for_copy = saved_view_context;

    // Reject `[...]` indexing of a possibly-null container (strict null checks).
    xa_check_nullable_access(ctx, node, container, "index access");

    /* Visit the index expression so variable references get their symbol_id resolved */
    XrType *index_type = NULL;
    if (ig->index) {
        index_type = xa_visit_infer_expr(ctx, ig->index);
    }

    if (container && XR_TYPE_IS_ERROR(container))
        return container;
    if (index_type && XR_TYPE_IS_ERROR(index_type))
        return index_type;

    // FFI raw pointer subscript p[i] => *(p + i): yields the pointee type.
    // Dereferencing is unsafe (no bounds/null check), so it is only allowed
    // inside `unsafe { }`. Taking/holding/offsetting a pointer stays safe.
    if (XR_TYPE_IS_POINTER(container)) {
        if (ctx->unsafe_depth == 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                "raw pointer index `p[i]` must be inside an `unsafe { }` block", &loc);
        }
        if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type))
            add_index_type_error(ctx, node, index_type, xr_type_new_int(NULL));
        XrType *pointee = container->container.element_type;
        return pointee ? pointee : xr_type_new_unknown(NULL);
    }

    if ((XR_TYPE_IS_ARRAY(container) || XR_TYPE_IS_VIEW(container) || XR_TYPE_IS_SPAN(container)) &&
        container->container.element_type) {
        if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type))
            add_index_type_error(ctx, node, index_type, xr_type_new_int(NULL));
        return container->container.element_type;
    }
    if (container && container->kind == XR_KIND_FIXED_ARRAY &&
        container->fixed_array.element_type) {
        if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type))
            add_index_type_error(ctx, node, index_type, xr_type_new_int(NULL));
        return container->fixed_array.element_type;
    }
    if (XR_TYPE_IS_MAP(container) && container->map.value_type) {
        if (index_type && container->map.key_type && !XR_TYPE_IS_UNKNOWN(index_type) &&
            !xa_typecheck_assignable(container->map.key_type, index_type))
            add_index_type_error(ctx, node, index_type, container->map.key_type);
        return container->map.value_type;
    }
    if (xr_type_is_named_class(container, "Range")) {
        if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type))
            add_index_type_error(ctx, node, index_type, xr_type_new_int(NULL));
        return xr_type_new_int(ctx->analyzer->isolate);
    }
    if (XR_TYPE_IS_STRING(container)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
            "string does not support integer indexing or slice syntax; use runes(), bytes(), or "
            "slice(start, end)",
            &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    // Json subscript access: json["key"] → Json (or nullable schema field type if known).
    if (XR_TYPE_IS_JSON(container)) {
        // If index is a string literal and Json has schema, look up field type
        if (ig->index && ig->index->type == AST_LITERAL_STRING &&
            container->object.field_count > 0 && container->object.field_names &&
            container->object.field_types) {
            const char *key = ig->index->as.literal.raw_value.string_val;
            for (int i = 0; i < container->object.field_count; i++) {
                if (container->object.field_names[i] &&
                    strcmp(container->object.field_names[i], key) == 0) {
                    XrType *ft = container->object.field_types[i];
                    if (ft)
                        return xr_type_make_nullable(ctx->analyzer->isolate, ft);
                }
            }
            if (container->object.is_sealed) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg), "类型 '%s' 没有字段 '%s'",
                         object_shape_type_label(container), key);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                return xr_type_new_error(ctx->analyzer->isolate);
            }
        }
        // No schema or unknown key → result is Json (any JSON value including null)
        return xr_type_new_json(ctx->analyzer->isolate);
    }

    // Record subscript is only a fixed-field shorthand with string literal keys.
    if (XR_TYPE_IS_RECORD(container)) {
        if (ig->index && ig->index->type == AST_LITERAL_STRING && container->object.field_names &&
            container->object.field_types) {
            const char *key = ig->index->as.literal.raw_value.string_val;
            int field_idx = object_shape_field_index(container, key);
            if (field_idx >= 0)
                return container->object.field_types[field_idx]
                           ? container->object.field_types[field_idx]
                           : xr_type_new_unknown(NULL);
            if (container->object.is_sealed) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg), "类型 '%s' 没有字段 '%s'",
                         object_shape_type_label(container), key);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                return xr_type_new_error(ctx->analyzer->isolate);
            }
            return xr_type_new_unknown(NULL);
        }
        if (container->object.is_sealed) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                "sealed Record index access requires a string literal key", &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
        return xr_type_new_unknown(NULL);
    }

    if (container && !XR_TYPE_IS_UNKNOWN(container)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[192];
        snprintf(msg, sizeof(msg), "type '%s' does not support indexed access",
                 xr_type_to_string(container));
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_tuple_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_error(NULL);

    TupleLiteralNode *tup = &node->as.tuple_literal;
    /* `()` is the unit literal — the unique value of the unit type
     * (XR_KIND_UNIT). It is *not* a 0-arity XR_KIND_TUPLE: keeping unit
     * its own kind lets the rest of the type system stay unchanged for
     * void-like contexts. */
    if (tup->count == 0)
        return xr_type_new_unit(ctx->analyzer->isolate);

    /* Detect spread elements upfront: their static arity decides the
     * final tuple arity, so we can no longer push expected-type slots
     * 1:1 onto child elements. Without spreads we keep the existing
     * bidirectional propagation. */
    bool has_spread = false;
    for (int i = 0; i < tup->count; i++) {
        if (tup->elements[i] && tup->elements[i]->type == AST_SPREAD_EXPR) {
            has_spread = true;
            break;
        }
    }

    XrType *saved_expected = ctx->expected_type;
    XrType *expected_tuple = NULL;
    if (!has_spread && saved_expected && XR_TYPE_IS_TUPLE(saved_expected) &&
        saved_expected->tuple.element_count == tup->count) {
        expected_tuple = saved_expected;
    }

    /* Two-pass build: collect per-slot types into a growable buffer.
     * Spreads of known tuple types contribute their element types one
     * by one; non-tuple spread sources emit an error and are skipped. */
    int cap = tup->count + 8;
    XrType **elem_types = (XrType **) xr_malloc(sizeof(XrType *) * (size_t) cap);
    if (!elem_types) {
        ctx->expected_type = saved_expected;
        return xr_type_new_error(ctx->analyzer->isolate);
    }
    int slot = 0;
    bool poisoned = false;

    for (int i = 0; i < tup->count; i++) {
        AstNode *child = tup->elements[i];
        if (!child)
            continue;

        if (child->type == AST_SPREAD_EXPR) {
            ctx->expected_type = NULL;
            XrType *src = xa_visit_infer_expr(ctx, child->as.spread_expr.expr);
            if (src && XR_TYPE_IS_ERROR(src)) {
                poisoned = true;
                continue;
            }
            if (!src || !XR_TYPE_IS_TUPLE(src)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = child->line, .column = child->column};
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Spread '...' source must be a tuple of statically known arity, got '%s'",
                         src ? xr_type_to_string(src) : "<unknown>");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                poisoned = true;
                continue;
            }
            xa_check_span_value_escape(ctx, child, src, "spread Slice view into tuple literal");
            int ec = src->tuple.element_count;
            if (slot + ec > cap) {
                int new_cap = (slot + ec + 8) * 2;
                XrType **resized =
                    (XrType **) xr_realloc(elem_types, sizeof(XrType *) * (size_t) new_cap);
                if (!resized) {
                    xr_free(elem_types);
                    ctx->expected_type = saved_expected;
                    return xr_type_new_error(ctx->analyzer->isolate);
                }
                elem_types = resized;
                cap = new_cap;
            }
            for (int j = 0; j < ec; j++)
                elem_types[slot++] = src->tuple.element_types[j];
            continue;
        }

        ctx->expected_type = expected_tuple ? expected_tuple->tuple.element_types[i] : NULL;
        if (slot >= cap) {
            int new_cap = cap * 2;
            XrType **resized =
                (XrType **) xr_realloc(elem_types, sizeof(XrType *) * (size_t) new_cap);
            if (!resized) {
                xr_free(elem_types);
                ctx->expected_type = saved_expected;
                return xr_type_new_error(ctx->analyzer->isolate);
            }
            elem_types = resized;
            cap = new_cap;
        }
        XrType *elem = xa_visit_infer_expr(ctx, child);
        xa_check_span_value_escape(ctx, child, elem, "store Slice view in tuple literal");
        if (elem && XR_TYPE_IS_ERROR(elem))
            poisoned = true;
        elem_types[slot++] = elem;
    }
    ctx->expected_type = saved_expected;

    if (poisoned) {
        xr_free(elem_types);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    if (slot == 0) {
        xr_free(elem_types);
        return xr_type_new_unit(ctx->analyzer->isolate);
    }
    XrType *result = xr_type_new_tuple(ctx->analyzer->isolate, elem_types, slot);
    xr_free(elem_types);
    return result ? result : xr_type_new_unknown(NULL);
}

XrType *xa_visit_array_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_error(NULL);

    ArrayLiteralNode *arr = &node->as.array_literal;
    if (arr->is_fixed_bytes_literal) {
        XrType *byte_type = xr_type_new_int_width(ctx->analyzer->isolate, XR_NATIVE_U8);
        XrType *saved_expected = ctx->expected_type;
        bool poisoned = false;
        for (int i = 0; i < arr->count; i++) {
            ctx->expected_type = byte_type;
            XrType *elem = xa_visit_infer_expr(ctx, arr->elements[i]);
            if (elem && XR_TYPE_IS_ERROR(elem))
                poisoned = true;
        }
        ctx->expected_type = saved_expected;
        if (poisoned)
            return xr_type_new_error(ctx->analyzer->isolate);
        return xr_type_new_fixed_array(ctx->analyzer->isolate, byte_type, arr->count);
    }
    if (arr->is_repeat) {
        int repeat_count = 0;
        const char *repeat_err = NULL;
        if (!xa_array_repeat_count_const_expr(ctx, arr->repeat_count, &repeat_count, &repeat_err)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = arr->repeat_count ? arr->repeat_count->line : node->line,
                              .column =
                                  arr->repeat_count ? arr->repeat_count->column : node->column};
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "fixed array repeat count must be a positive compile-time integer "
                     "expression%s%s",
                     repeat_err ? ": " : "", repeat_err ? repeat_err : "");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }

        XrType *elem_expected = NULL;
        if (ctx->expected_type && ctx->expected_type->kind == XR_KIND_FIXED_ARRAY &&
            ctx->expected_type->fixed_array.element_type) {
            elem_expected = ctx->expected_type->fixed_array.element_type;
            if (repeat_count > 0 && repeat_count != ctx->expected_type->fixed_array.length) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "fixed array repeat count mismatch: expected %d element(s), got %d",
                         ctx->expected_type->fixed_array.length, repeat_count);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        }

        XrType *saved_expected = ctx->expected_type;
        ctx->expected_type = elem_expected;
        XrType *elem_type = xa_visit_infer_expr(ctx, arr->repeat_value);
        ctx->expected_type = saved_expected;
        xa_check_span_value_escape(ctx, arr->repeat_value, elem_type,
                                   "repeat Slice view in array literal");
        XrType *count_type = xa_visit_infer_expr(ctx, arr->repeat_count);
        if ((elem_type && XR_TYPE_IS_ERROR(elem_type)) ||
            (count_type && XR_TYPE_IS_ERROR(count_type)))
            return xr_type_new_error(ctx->analyzer->isolate);
        if (count_type && !XR_TYPE_IS_UNKNOWN(count_type) && !XR_TYPE_IS_INT(count_type)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = arr->repeat_count->line,
                              .column = arr->repeat_count->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH,
                                       "fixed array repeat count must have type int", &loc);
        }

        if (elem_expected && elem_type && !XR_TYPE_IS_UNKNOWN(elem_type) &&
            !xa_typecheck_assignable(elem_expected, elem_type)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = arr->repeat_value ? arr->repeat_value->line : node->line,
                              .column =
                                  arr->repeat_value ? arr->repeat_value->column : node->column};
            char msg[192];
            snprintf(msg, sizeof(msg), "fixed array element has type '%s', expected '%s'",
                     xr_type_to_string(elem_type), xr_type_to_string(elem_expected));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
        if (ctx->expected_type && ctx->expected_type->kind == XR_KIND_FIXED_ARRAY)
            return ctx->expected_type;
        if (!elem_type || XR_TYPE_IS_UNKNOWN(elem_type) || repeat_count <= 0)
            return repeat_count <= 0 ? xr_type_new_error(ctx->analyzer->isolate)
                                     : xr_type_new_unknown(ctx->analyzer->isolate);
        return xr_type_new_fixed_array(ctx->analyzer->isolate, elem_type, repeat_count);
    }

    if (ctx->expected_type && ctx->expected_type->kind == XR_KIND_FIXED_ARRAY &&
        ctx->expected_type->fixed_array.element_type) {
        XrType *fixed = ctx->expected_type;
        XrType *elem_expected = fixed->fixed_array.element_type;
        bool poisoned = false;
        if (arr->count != fixed->fixed_array.length) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "fixed array literal length mismatch: expected %d element(s), got %d",
                     fixed->fixed_array.length, arr->count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
        XrType *saved_expected = ctx->expected_type;
        for (int i = 0; i < arr->count; i++) {
            AstNode *child = arr->elements[i];
            if (!child)
                continue;
            if (child->type == AST_SPREAD_EXPR) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = child->line, .column = child->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH,
                                           "fixed array literal cannot use spread '...'", &loc);
                continue;
            }
            ctx->expected_type = elem_expected;
            XrType *elem_type = xa_visit_infer_expr(ctx, child);
            xa_check_span_value_escape(ctx, child, elem_type,
                                       "store Slice view in fixed array literal");
            if (elem_type && XR_TYPE_IS_ERROR(elem_type)) {
                poisoned = true;
                continue;
            }
            if (elem_type && !XR_TYPE_IS_UNKNOWN(elem_type) &&
                !xa_typecheck_assignable(elem_expected, elem_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = child->line, .column = child->column};
                char msg[192];
                snprintf(msg, sizeof(msg), "fixed array element has type '%s', expected '%s'",
                         xr_type_to_string(elem_type), xr_type_to_string(elem_expected));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        }
        ctx->expected_type = saved_expected;
        if (poisoned)
            return xr_type_new_error(ctx->analyzer->isolate);
        return fixed;
    }

    xa_freestanding_report_unavailable(
        ctx, node, "Array literal",
        "dynamic arrays require hosted allocation; use a target-typed fixed array literal when a "
        "no-heap layout is required");

    if (ctx->expected_type && XR_TYPE_IS_JSON(ctx->expected_type)) {
        XrType *saved_expected = ctx->expected_type;
        XrType *json_type = xr_type_new_json(ctx->analyzer->isolate);
        bool poisoned = false;
        for (int i = 0; i < arr->count; i++) {
            AstNode *child = arr->elements[i];
            if (!child)
                continue;
            if (child->type == AST_SPREAD_EXPR) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = child->line, .column = child->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH,
                                           "Json array literal cannot spread an external Array; "
                                           "use Json.encode(array) at the boundary",
                                           &loc);
                continue;
            }
            ctx->expected_type = json_type;
            XrType *elem_type = xa_visit_infer_expr(ctx, child);
            xa_check_span_value_escape(ctx, child, elem_type,
                                       "store Slice view in Json array literal");
            if (elem_type && XR_TYPE_IS_ERROR(elem_type)) {
                poisoned = true;
                continue;
            }
            if (elem_type && !xr_type_is_json_field_compatible(elem_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = child->line, .column = child->column};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Json array element has type '%s'; use Json.encode(...) for external "
                         "Xray values",
                         xr_type_to_string(elem_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        }
        ctx->expected_type = saved_expected;
        if (poisoned)
            return xr_type_new_error(ctx->analyzer->isolate);
        return json_type;
    }

    XrType *target_elem_type = NULL;
    if (arr->count == 0) {
        // Empty array: use expected type if available
        if (ctx->expected_type &&
            (XR_TYPE_IS_ARRAY(ctx->expected_type) || XR_TYPE_IS_VIEW(ctx->expected_type) ||
             XR_TYPE_IS_SPAN(ctx->expected_type)) &&
            ctx->expected_type->container.element_type) {
            return xr_type_new_array(ctx->analyzer->isolate,
                                     ctx->expected_type->container.element_type);
        }
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        XaInferVar *var = xa_infer_var_new(ctx, "empty array element", &loc);
        return xa_infer_var_report_unsolved(
            ctx, var,
            "cannot infer element type for empty array literal; add an explicit Array<T> "
            "annotation or contextual type");
    }

    // Propagate expected element type to children. A `...spread` element
    // contributes a whole array, so its source is checked against the array
    // type (T[]); plain elements are checked against the element type (T).
    XrType *saved_expected = ctx->expected_type;
    XrType *expected_array = NULL;
    if (ctx->expected_type &&
        (XR_TYPE_IS_ARRAY(ctx->expected_type) || XR_TYPE_IS_VIEW(ctx->expected_type) ||
         XR_TYPE_IS_SPAN(ctx->expected_type)) &&
        ctx->expected_type->container.element_type) {
        target_elem_type = ctx->expected_type->container.element_type;
        expected_array = XR_TYPE_IS_ARRAY(ctx->expected_type)
                             ? ctx->expected_type
                             : xr_type_new_array(ctx->analyzer->isolate,
                                                 ctx->expected_type->container.element_type);
    }

    bool use_target_elem_type = (target_elem_type != NULL);
    XrType *elem_type = NULL;
    bool poisoned = false;

    for (int i = 0; i < arr->count; i++) {
        AstNode *child = arr->elements[i];
        XrType *contributed = NULL;
        if (child && child->type == AST_SPREAD_EXPR) {
            // Spread source must be an array; splice its element type in.
            ctx->expected_type = expected_array;
            XrType *src = xa_visit_infer_expr(ctx, child->as.spread_expr.expr);
            if (src && XR_TYPE_IS_ERROR(src)) {
                contributed = src;
                poisoned = true;
            } else if (src &&
                       (XR_TYPE_IS_ARRAY(src) || XR_TYPE_IS_VIEW(src) || XR_TYPE_IS_SPAN(src)) &&
                       src->container.element_type) {
                contributed = src->container.element_type;
                xa_check_span_value_escape(ctx, child, src, "spread Slice view into array literal");
            } else {
                if (src && !XR_TYPE_IS_UNKNOWN(src)) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = child->line, .column = child->column};
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "array spread '...' source must be an array, got '%s'",
                             xr_type_to_string(src));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
                contributed = src && XR_TYPE_IS_UNKNOWN(src)
                                  ? xr_type_new_unknown(NULL)
                                  : xr_type_new_error(ctx->analyzer->isolate);
                if (XR_TYPE_IS_ERROR(contributed))
                    poisoned = true;
            }
        } else {
            ctx->expected_type = target_elem_type;
            contributed = xa_visit_infer_expr(ctx, child);
            xa_check_span_value_escape(ctx, child, contributed,
                                       "store Slice view in array literal");
            if (contributed && XR_TYPE_IS_ERROR(contributed))
                poisoned = true;
        }

        if (use_target_elem_type && contributed && !XR_TYPE_IS_UNKNOWN(contributed) &&
            !xa_typecheck_assignable(target_elem_type, contributed)) {
            use_target_elem_type = false;
        }
        if (!elem_type) {
            elem_type = contributed;
        } else if (contributed && !xr_type_equals(elem_type, contributed)) {
            elem_type = xr_type_union(ctx->analyzer->isolate, elem_type, contributed);
        }
    }

    ctx->expected_type = saved_expected;
    if (poisoned)
        return xr_type_new_error(ctx->analyzer->isolate);
    if (use_target_elem_type) {
        return xr_type_new_array(ctx->analyzer->isolate, target_elem_type);
    }
    return xr_type_new_array(ctx->analyzer->isolate,
                             elem_type ? elem_type : xr_type_new_unknown(NULL));
}

XrType *xa_visit_map_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_error(NULL);

    xa_freestanding_report_unavailable(ctx, node, "Map literal",
                                       "dynamic containers require hosted allocation");

    MapLiteralNode *map = &node->as.map_literal;
    if (map->count == 0) {
        // Empty map: use expected type if available
        if (ctx->expected_type && XR_TYPE_IS_MAP(ctx->expected_type) &&
            ctx->expected_type->map.key_type && ctx->expected_type->map.value_type &&
            !XR_TYPE_IS_UNKNOWN(ctx->expected_type->map.key_type) &&
            !XR_TYPE_IS_UNKNOWN(ctx->expected_type->map.value_type)) {
            XrType *ek = ctx->expected_type->map.key_type;
            XrType *ev = ctx->expected_type->map.value_type;
            return xr_type_new_map(ctx->analyzer->isolate, ek, ev);
        }
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        XaInferVar *var = xa_infer_var_new(ctx, "empty map key/value", &loc);
        return xa_infer_var_report_unsolved(
            ctx, var,
            "cannot infer key/value types for empty map literal; add an explicit Map<K, V> "
            "annotation or contextual type");
    }

    // Propagate expected key/value types to children
    XrType *saved_expected = ctx->expected_type;
    XrType *target_key_type = NULL;
    XrType *target_value_type = NULL;
    if (ctx->expected_type && XR_TYPE_IS_MAP(ctx->expected_type)) {
        target_key_type = ctx->expected_type->map.key_type;
        target_value_type = ctx->expected_type->map.value_type;
    }

    // Infer key/value types from first element
    ctx->expected_type = target_key_type;
    XrType *key_type = xa_visit_infer_expr(ctx, map->keys[0]);
    xa_check_span_value_escape(ctx, map->keys[0], key_type, "store Slice view as map literal key");
    ctx->expected_type = target_value_type;
    XrType *val_type = xa_visit_infer_expr(ctx, map->values[0]);
    xa_check_span_value_escape(ctx, map->values[0], val_type, "store Slice view in map literal");

    // Union with remaining elements (same pattern as array_literal)
    for (int i = 1; i < map->count; i++) {
        ctx->expected_type = target_key_type;
        XrType *k = xa_visit_infer_expr(ctx, map->keys[i]);
        xa_check_span_value_escape(ctx, map->keys[i], k, "store Slice view as map literal key");
        ctx->expected_type = target_value_type;
        XrType *v = xa_visit_infer_expr(ctx, map->values[i]);
        xa_check_span_value_escape(ctx, map->values[i], v, "store Slice view in map literal");
        if (!xr_type_equals(key_type, k)) {
            key_type = xr_type_union(ctx->analyzer->isolate, key_type, k);
        }
        if (!xr_type_equals(val_type, v)) {
            val_type = xr_type_union(ctx->analyzer->isolate, val_type, v);
        }
    }

    ctx->expected_type = saved_expected;
    if ((key_type && XR_TYPE_IS_ERROR(key_type)) || (val_type && XR_TYPE_IS_ERROR(val_type)))
        return xr_type_new_error(ctx->analyzer->isolate);
    XrType *result = xr_type_new_map(ctx->analyzer->isolate, key_type, val_type);
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    xa_validate_hashable_key_type(ctx, result, NULL, "map literal", &loc);
    return result;
}

// Add (name, type) to the unified field list, overriding the type if `name`
// is already present (later object-literal entries / spread parts win).
static void object_union_add(const char **names, XrType **types, int *n, const char *name,
                             XrType *type) {
    for (int k = 0; k < *n; k++) {
        if (names[k] && name && strcmp(names[k], name) == 0) {
            types[k] = type;
            return;
        }
    }
    names[*n] = name;
    types[*n] = type;
    (*n)++;
}

XrType *xa_visit_object_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    xa_freestanding_report_unavailable(ctx, node, "object literal",
                                       "use a declared struct literal for fixed layout data");

    ObjectLiteralNode *obj = &node->as.object_literal;
    XrType *expected = ctx->expected_type;
    bool json_context = expected && XR_TYPE_IS_JSON(expected);
    bool record_context = expected && XR_TYPE_IS_RECORD(expected);
    bool result_is_json = json_context;

    if (obj->count == 0) {
        if (json_context)
            return xr_type_new_json(ctx->analyzer->isolate);
        if (!record_context) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                "empty object literal '{}' requires an explicit Record or Json context", &loc);
            return xr_type_new_error(ctx->analyzer->isolate);
        }
        return xr_type_new_record_with_fields(ctx->analyzer->isolate, NULL, NULL, 0,
                                              expected->object.is_sealed);
    }

    // Pass 1: infer each entry's contributing type exactly once (a spread
    // contributes its source object, a normal entry contributes its value),
    // and bound the unified field set. The result field set is the union of
    // every statically-known source field plus each literal key.
    XrType **entry_types = (XrType **) xr_malloc(sizeof(XrType *) * (size_t) obj->count);
    int cap = 0;
    for (int i = 0; i < obj->count; i++) {
        AstNode *val = obj->values[i];
        if (val && val->type == AST_SPREAD_EXPR) {
            XrType *src = xa_visit_infer_expr(ctx, val->as.spread_expr.expr);
            entry_types[i] = src;
            xa_check_span_value_escape(ctx, val, src,
                                       "spread Slice view fields into object literal");
            bool src_ok =
                result_is_json ? (src && XR_TYPE_IS_JSON(src)) : (src && XR_TYPE_IS_RECORD(src));
            if (src_ok) {
                cap += src->object.field_count;
            } else if (src && !XR_TYPE_IS_UNKNOWN(src)) {
                XrLocation loc = {.file = ctx->file_path, .line = val->line, .column = val->column};
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "object spread '...' source must be a %s object, got '%s'",
                         result_is_json ? "Json" : "Record", xr_type_to_string(src));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        } else {
            bool is_computed = obj->computed && obj->computed[i];
            if (is_computed && !result_is_json) {
                XrLocation loc = {.file = ctx->file_path,
                                  .line = obj->keys[i] ? obj->keys[i]->line : node->line,
                                  .column = obj->keys[i] ? obj->keys[i]->column : node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH,
                                           "sealed Record literal requires static field names; "
                                           "use an explicit Json context for computed keys",
                                           &loc);
            }
            XrType *saved_expected = ctx->expected_type;
            XrType *field_expected = NULL;
            const char *static_name = NULL;
            if (!is_computed && obj->keys[i]) {
                if (obj->keys[i]->type == AST_VARIABLE)
                    static_name = obj->keys[i]->as.variable.name;
                else if (obj->keys[i]->type == AST_LITERAL_STRING)
                    static_name = obj->keys[i]->as.literal.raw_value.string_val;
            }
            if (json_context) {
                field_expected = xr_type_new_json(ctx->analyzer->isolate);
            } else if (record_context && static_name && expected->object.field_names &&
                       expected->object.field_types) {
                int idx = object_shape_field_index(expected, static_name);
                if (idx >= 0)
                    field_expected = expected->object.field_types[idx];
            }
            ctx->expected_type = field_expected;
            entry_types[i] = xa_visit_infer_expr(ctx, obj->values[i]);
            xa_check_span_value_escape(ctx, obj->values[i], entry_types[i],
                                       "store Slice view in object literal");
            ctx->expected_type = saved_expected;
            cap += 1;

            if (result_is_json && entry_types[i] &&
                !xr_type_is_json_field_compatible(entry_types[i])) {
                const char *fname = "<computed>";
                if (!(obj->computed && obj->computed[i]) && obj->keys[i]) {
                    if (obj->keys[i]->type == AST_VARIABLE)
                        fname = obj->keys[i]->as.variable.name;
                    else if (obj->keys[i]->type == AST_LITERAL_STRING)
                        fname = obj->keys[i]->as.literal.raw_value.string_val;
                }
                XrLocation loc = {.file = ctx->file_path,
                                  .line = obj->values[i]->line,
                                  .column = obj->values[i]->column};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Json field '%s' has type '%s'; use Json.encode(...) for external "
                         "Xray values",
                         fname, xr_type_to_string(entry_types[i]));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        }
    }
    if (cap < 1)
        cap = 1;

    // Pass 2: build the unified field set in first-appearance order.
    const char **field_names = (const char **) xr_malloc(sizeof(char *) * (size_t) cap);
    XrType **field_types = (XrType **) xr_malloc(sizeof(XrType *) * (size_t) cap);
    int n = 0;
    for (int i = 0; i < obj->count; i++) {
        AstNode *val = obj->values[i];
        if (val && val->type == AST_SPREAD_EXPR) {
            XrType *src = entry_types[i];
            bool src_ok =
                result_is_json ? (src && XR_TYPE_IS_JSON(src)) : (src && XR_TYPE_IS_RECORD(src));
            if (src_ok && src->object.field_count > 0 && src->object.field_names) {
                for (int j = 0; j < src->object.field_count; j++) {
                    const char *fname = src->object.field_names[j];
                    if (!fname)
                        continue;
                    XrType *ftype = src->object.field_types ? src->object.field_types[j] : NULL;
                    object_union_add(field_names, field_types, &n, fname, ftype);
                }
            }
            // Dynamic Json source: fields unknown at compile time; they are merged
            // at runtime and not part of the static shape.
        } else {
            bool is_computed = obj->computed && obj->computed[i];
            const char *fname = NULL;
            if (!is_computed && obj->keys[i]) {
                if (obj->keys[i]->type == AST_VARIABLE)
                    fname = obj->keys[i]->as.variable.name;
                else if (obj->keys[i]->type == AST_LITERAL_STRING)
                    fname = obj->keys[i]->as.literal.raw_value.string_val;
            }
            if (fname)
                object_union_add(field_names, field_types, &n, fname, entry_types[i]);
        }
    }

    XrType *type;
    if (result_is_json && n == 0)
        type = xr_type_new_json(ctx->analyzer->isolate);
    else if (result_is_json)
        type = xr_type_new_json_with_fields(ctx->analyzer->isolate, field_names, field_types, n,
                                            false);
    else
        type = xr_type_new_record_with_fields(ctx->analyzer->isolate, field_names, field_types, n,
                                              record_context ? expected->object.is_sealed : true);
    xr_free(entry_types);
    xr_free(field_names);
    xr_free(field_types);

    return type;
}

XrType *xa_visit_new_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_error(NULL);

    xa_freestanding_report_unavailable(ctx, node, "class construction",
                                       "use structs or explicit raw-memory APIs in this profile");

    NewExprNode *ne = &node->as.new_expr;

    /* Visit argument expressions so their types are resolved. */
    for (int i = 0; i < ne->arg_count; i++) {
        if (ne->arguments[i])
            xa_visit_infer_expr(ctx, ne->arguments[i]);
    }

    /* Builtin heap types: return the correct container/channel type
     * directly, bypassing class-symbol lookup. Container construction
     * requires explicit type arguments: Map<string, int>(), Channel<int>(). */
    if (ne->class_name && !ne->module_name) {
        XrVMRuntime *X = ctx->analyzer->isolate;
        const char *cn = ne->class_name;
        XrType *bt = NULL;

        int required_type_args = -1;
        if (strcmp(cn, "Map") == 0 || strcmp(cn, "WeakMap") == 0)
            required_type_args = 2;
        else if (strcmp(cn, "Array") == 0 || strcmp(cn, "Set") == 0 || strcmp(cn, "WeakSet") == 0 ||
                 strcmp(cn, "Channel") == 0)
            required_type_args = 1;
        if (required_type_args >= 0 && ne->type_arg_count != required_type_args) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "generic constructor '%s' expects %d type argument%s, got %d", cn,
                     required_type_args, required_type_args == 1 ? "" : "s", ne->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
            return xr_type_new_error(X);
        }

        /* Resolve explicit type arguments if present */
        XrType *ta[8] = {0};
        int tac = ne->type_arg_count > 8 ? 8 : ne->type_arg_count;
        for (int i = 0; i < tac; i++)
            ta[i] = ne->type_args[i] ? xr_tref_resolve_in_analyzer(ctx->analyzer, ne->type_args[i])
                                     : xr_type_new_error(NULL);
        bool poisoned_type_arg = false;
        for (int i = 0; i < tac; i++) {
            if (xa_reject_error_type_success_type(ctx->analyzer, ta[i], "generic type argument", cn,
                                                  node ? node->line : 0, node ? node->column : 0))
                poisoned_type_arg = true;
        }
        if (poisoned_type_arg)
            return xr_type_new_error(NULL);

        if (strcmp(cn, "Map") == 0 || strcmp(cn, "WeakMap") == 0) {
            bt = xr_type_new_map(X, ta[0], ta[1]);
            if (strcmp(cn, "WeakMap") == 0)
                bt->is_weak = true;
        } else if (strcmp(cn, "Array") == 0) {
            bt = xr_type_new_array(X, ta[0]);
        } else if (strcmp(cn, "Set") == 0 || strcmp(cn, "WeakSet") == 0) {
            bt = xr_type_new(X, XR_KIND_SET);
            if (bt) {
                bt->container.element_type = ta[0];
                if (strcmp(cn, "WeakSet") == 0)
                    bt->is_weak = true;
            }
        } else if (strcmp(cn, "Channel") == 0) {
            bt = xr_type_new(X, XR_KIND_CHANNEL);
            if (bt)
                bt->container.element_type = ta[0];
        } else if (strcmp(cn, "StringBuilder") == 0) {
            bt = xr_type_new_named_instance(X, "StringBuilder");
        } else if (strcmp(cn, "Atomic") == 0) {
            XrType *et = tac >= 1 ? ta[0] : NULL;
            if (!et && ne->arg_count > 0 && ne->arguments[0]) {
                et = xa_visit_infer_expr(ctx, ne->arguments[0]);
            }
            if (!et)
                et = xr_type_new_unknown(X);
            XrType **arg_copy = (XrType **) xr_malloc(sizeof(XrType *));
            if (arg_copy) {
                arg_copy[0] = et;
                bt = xr_type_new_generic_instance(X, "Atomic", NULL, arg_copy, 1);
            }
        }
        if (bt) {
            if ((strcmp(cn, "Array") == 0 && ne->arg_count >= 2) ||
                ((strcmp(cn, "Set") == 0 || strcmp(cn, "WeakSet") == 0) && ne->arg_count >= 1)) {
                int value_slot = strcmp(cn, "Array") == 0 ? 1 : 0;
                AstNode *value_arg = ne->arguments ? ne->arguments[value_slot] : NULL;
                XrType *value_type =
                    value_arg ? xa_analyzer_get_node_type(ctx->analyzer, value_arg) : NULL;
                if (!value_type && value_arg)
                    value_type = xa_visit_infer_expr(ctx, value_arg);
                if (value_type && xa_type_contains_span_view(value_type)) {
                    char context[96];
                    snprintf(context, sizeof(context), "store Slice view in %s constructor", cn);
                    xa_check_span_value_escape(ctx, value_arg ? value_arg : node, value_type,
                                               context);
                }
            }
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_validate_hashable_key_type(ctx, bt, NULL, "constructor type", &loc);
            return bt;
        }
    }

    // Look up class symbol to get XrClassInfo
    // Try current scope first, then global scope
    XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->current_scope, ne->class_name);
    if (!class_sym) {
        class_sym = xa_scope_lookup(ctx->analyzer->global_scope, ne->class_name);
    }

    XrClassInfo *class_info = NULL;
    XaSymbolLinks *class_links = NULL;
    if (class_sym && class_sym->kind == XA_SYM_CLASS) {
        class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
        if (class_links && class_links->class_info) {
            class_info = class_links->class_info;
        }
    }

    // Caller-side default argument filling for constructors (C1): complete
    // omitted trailing constructor arguments with session-cloned default
    // expressions so they are evaluated at the construction site rather than
    // via a runtime null sentinel inside the constructor body.
    if (class_info) {
        xa_check_constructor_visibility(ctx, node, class_info);
        XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
        XaSymbolLinks *ctor_links = ctor ? xa_analyzer_get_links(ctx->analyzer, ctor) : NULL;
        if (ctor_links && ctor_links->param_defaults && ctor_links->param_count > ne->arg_count) {
            int pc = ctor_links->param_count;
            bool can_complete = true;
            for (int i = 0; i < ne->arg_count; i++) {
                if (ne->arguments[i] && ne->arguments[i]->type == AST_SPREAD_EXPR) {
                    can_complete = false;
                    break;
                }
            }
            for (int i = ne->arg_count; can_complete && i < pc; i++) {
                if (!ctor_links->param_defaults[i])
                    can_complete = false;
            }
            if (can_complete) {
                XrCompilerSession *sess =
                    xr_compiler_session_current_for_isolate(ctx->analyzer->isolate);
                AstNode **new_args = (AstNode **) xr_calloc((size_t) pc, sizeof(AstNode *));
                XrCallArgAccess *new_accesses =
                    (XrCallArgAccess *) xr_calloc((size_t) pc, sizeof(XrCallArgAccess));
                if (new_args && new_accesses) {
                    for (int i = 0; i < ne->arg_count; i++)
                        new_args[i] = ne->arguments[i];
                    for (int i = 0; i < ne->arg_count; i++)
                        new_accesses[i] =
                            ne->arg_accesses ? ne->arg_accesses[i] : XR_CALL_ARG_VALUE;
                    for (int i = ne->arg_count; i < pc; i++) {
                        AstNode *clone = xr_ast_clone_session(ctor_links->param_defaults[i], sess);
                        new_args[i] = clone;
                        if (clone)
                            xa_visit_infer_expr(ctx, clone);
                    }
                    ne->arguments = new_args;
                    ne->arg_accesses = new_accesses;
                    ne->arg_count = pc;
                } else {
                    xr_free(new_args);
                    xr_free(new_accesses);
                }
            }
        }
        XrType *ctor_type = ctor_links ? ctor_links->type : NULL;
        if (ctor_type && XR_TYPE_IS_FUNCTION(ctor_type)) {
            int ctor_pc = ctor_type->function.param_count;
            int check_count = ctor_pc < ne->arg_count ? ctor_pc : ne->arg_count;
            for (int i = 0; i < check_count; i++) {
                XrCallArgAccess access = ne->arg_accesses ? ne->arg_accesses[i] : XR_CALL_ARG_VALUE;
                xa_check_arg_access_authorization(ctx, node,
                                                  ne->arguments ? ne->arguments[i] : NULL, access,
                                                  i, xr_type_function_param_mode(ctor_type, i));
            }
        }
    }

    // Check type argument count matches type parameter count
    if (ne->type_arg_count > 0) {
        int expected = class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
        if (expected > 0 && ne->type_arg_count != expected) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(msg, sizeof(msg), "Generic class '%s' expects %d type argument(s), but got %d",
                     ne->class_name, expected, ne->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
        }
    }

    // If we have generic type arguments, create a generic instance type
    if (ne->type_arg_count > 0) {
        // Resolve XrTypeRef** to XrType** for runtime use
        XrType *resolved_targs_buf[8];
        XrType **resolved_targs = (ne->type_arg_count <= 8)
                                      ? resolved_targs_buf
                                      : xr_malloc(sizeof(XrType *) * (size_t) ne->type_arg_count);
        for (int i = 0; i < ne->type_arg_count; i++)
            resolved_targs[i] = ne->type_args[i]
                                    ? xr_tref_resolve_in_analyzer(ctx->analyzer, ne->type_args[i])
                                    : xr_type_new_unknown(NULL);
        bool poisoned_type_arg = false;
        for (int i = 0; i < ne->type_arg_count; i++) {
            if (xa_reject_error_type_success_type(ctx->analyzer, resolved_targs[i],
                                                  "generic type argument", ne->class_name,
                                                  node ? node->line : 0, node ? node->column : 0)) {
                poisoned_type_arg = true;
            }
        }
        if (poisoned_type_arg) {
            if (resolved_targs != resolved_targs_buf)
                xr_free(resolved_targs);
            return xr_type_new_error(NULL);
        }
        xa_check_span_generic_class_type_args(ctx, node, ne->class_name, resolved_targs,
                                              ne->type_arg_count);

        // Check constructor argument types against substituted parameter types
        if (class_info && class_links && ne->arg_count > 0) {
            int type_param_count = xa_symbol_links_get_type_param_count(class_links);
            if (type_param_count > 0 && type_param_count == ne->type_arg_count) {
                XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
                if (ctor && ctor->kind == XA_SYM_METHOD) {
                    XaSymbolLinks *ctor_links = xa_analyzer_get_links(ctx->analyzer, ctor);
                    if (ctor_links && ctor_links->type && XR_TYPE_IS_FUNCTION(ctor_links->type)) {
                        // Build param_names array
                        const char *param_names_buf[8];
                        const char **param_names =
                            (type_param_count <= 8)
                                ? param_names_buf
                                : xr_malloc(sizeof(const char *) * type_param_count);
                        for (int i = 0; i < type_param_count; i++) {
                            param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                        }

                        int ctor_pc = ctor_links->type->function.param_count;
                        int check_count = ctor_pc < ne->arg_count ? ctor_pc : ne->arg_count;

                        for (int i = 0; i < check_count; i++) {
                            XrType *expected = xr_type_function_param_type(ctor_links->type, i);
                            if (!expected || XR_TYPE_IS_UNKNOWN(expected))
                                continue;
                            // Substitute T -> actual type arg
                            XrType *resolved =
                                xr_type_substitute(ctx->analyzer->isolate, expected, param_names,
                                                   resolved_targs, ne->type_arg_count);
                            if (resolved && !XR_TYPE_IS_UNKNOWN(resolved)) {
                                XrType *arg_type = xa_visit_infer_expr(ctx, ne->arguments[i]);
                                if (arg_type && !xa_typecheck_assignable(resolved, arg_type)) {
                                    XrLocation loc = {.file = ctx->file_path,
                                                      .line = node->line,
                                                      .column = node->column};
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "Type '%s' is not assignable to parameter type '%s' "
                                             "in new %s<>()",
                                             xr_type_to_string(arg_type),
                                             xr_type_to_string(resolved), ne->class_name);
                                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                                                               &loc);
                                }
                            }
                        }
                        if (param_names != param_names_buf)
                            xr_free((void *) param_names);
                    }
                }
            }
        }
        XrType *gi = xr_type_new_generic_instance(ctx->analyzer->isolate, ne->class_name,
                                                  class_info, resolved_targs, ne->type_arg_count);
        if (resolved_targs != resolved_targs_buf)
            xr_free(resolved_targs);
        return gi;
    }

    // Infer type arguments from constructor parameters: new Box(42) -> Box<int>
    if (class_links && ne->arg_count > 0) {
        int type_param_count = xa_symbol_links_get_type_param_count(class_links);
        if (type_param_count > 0 && class_info) {
            // Look up constructor
            XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
            if (ctor && ctor->kind == XA_SYM_METHOD) {
                XaSymbolLinks *ctor_links = xa_analyzer_get_links(ctx->analyzer, ctor);
                if (ctor_links && ctor_links->type && XR_TYPE_IS_FUNCTION(ctor_links->type)) {
                    int ctor_param_count = ctor_links->type->function.param_count;

                    // Build type parameter names
                    const char **param_names = xr_malloc(sizeof(const char *) * type_param_count);
                    for (int i = 0; i < type_param_count; i++) {
                        param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                    }

                    // Infer type arguments from constructor arguments
                    XrType **inferred_args = xr_malloc(sizeof(XrType *) * type_param_count);
                    bool all_inferred = true;

                    for (int i = 0; i < type_param_count; i++) {
                        inferred_args[i] = NULL;
                        const char *tp_name = param_names[i];

                        // Find constructor parameter that uses this type parameter
                        for (int j = 0; j < ctor_param_count && j < ne->arg_count; j++) {
                            XrType *pt = xr_type_function_param_type(ctor_links->type, j);
                            if (pt && (pt->kind == XR_KIND_TYPE_PARAM) && pt->type_param.name &&
                                strcmp(pt->type_param.name, tp_name) == 0) {
                                // Infer from argument type
                                inferred_args[i] = xa_visit_infer_expr(ctx, ne->arguments[j]);
                                break;
                            }
                        }

                        if (!inferred_args[i]) {
                            all_inferred = false;
                        }
                    }

                    // If all type parameters were inferred, create generic instance
                    if (all_inferred) {
                        bool poisoned_type_arg = false;
                        for (int i = 0; i < type_param_count; i++) {
                            if (xa_reject_error_type_success_type(
                                    ctx->analyzer, inferred_args[i], "generic type argument",
                                    ne->class_name, node ? node->line : 0,
                                    node ? node->column : 0)) {
                                poisoned_type_arg = true;
                            }
                        }
                        if (poisoned_type_arg) {
                            xr_free(param_names);
                            xr_free(inferred_args);
                            return xr_type_new_error(NULL);
                        }
                        xa_check_span_generic_class_type_args(ctx, node, ne->class_name,
                                                              inferred_args, type_param_count);
                        XrType *result = xr_type_new_generic_instance(
                            ctx->analyzer->isolate, ne->class_name, class_info, inferred_args,
                            type_param_count);
                        xr_free(param_names);
                        // Don't free inferred_args - it's now owned by the type
                        return result;
                    }

                    xr_free(param_names);
                    xr_free(inferred_args);
                }
            }
        }
    }

    // No type arguments - use regular instance or class type
    if (class_info) {
        XrType *inst_type = xr_type_new_instance(ctx->analyzer->isolate, class_info);
        // Propagate is_value_type from class declaration (struct)
        if (class_links && class_links->type && class_links->type->is_value_type) {
            inst_type->is_value_type = true;
        }
        return inst_type;
    }

    // Fallback: create instance type with class name (new always produces instances)
    if (ne->class_name) {
        XrType *inst_type = xr_type_new_named_instance(ctx->analyzer->isolate, ne->class_name);
        if (inst_type)
            inst_type->instance.class_ref = class_info;
        // Propagate is_value_type from class declaration (struct)
        if (class_links && class_links->type && class_links->type->is_value_type) {
            inst_type->is_value_type = true;
        }
        return inst_type;
    }
    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_struct_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    StructLiteralNode *sl = &node->as.struct_literal;
    const char *struct_name = sl->struct_name;

    // Look up struct symbol
    XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->current_scope, struct_name);
    if (!class_sym) {
        class_sym = xa_scope_lookup(ctx->analyzer->global_scope, struct_name);
    }

    XaSymbolLinks *links = NULL;
    XrClassInfo *class_info = NULL;
    if (class_sym && class_sym->kind == XA_SYM_CLASS) {
        links = xa_analyzer_get_links(ctx->analyzer, class_sym);
        class_info = links ? links->class_info : NULL;
    }

    XrType *resolved_targs_buf[8] = {0};
    XrType **resolved_targs = NULL;
    if (sl->type_arg_count > 0) {
        resolved_targs = (sl->type_arg_count <= 8)
                             ? resolved_targs_buf
                             : xr_malloc(sizeof(XrType *) * (size_t) sl->type_arg_count);
        if (resolved_targs) {
            for (int i = 0; i < sl->type_arg_count; i++) {
                resolved_targs[i] =
                    sl->type_args[i] ? xr_tref_resolve_in_analyzer(ctx->analyzer, sl->type_args[i])
                                     : xr_type_new_unknown(NULL);
            }
            bool poisoned_type_arg = false;
            for (int i = 0; i < sl->type_arg_count; i++) {
                if (xa_reject_error_type_success_type(
                        ctx->analyzer, resolved_targs[i], "generic type argument", struct_name,
                        node ? node->line : 0, node ? node->column : 0)) {
                    poisoned_type_arg = true;
                }
            }
            if (poisoned_type_arg) {
                if (resolved_targs != resolved_targs_buf)
                    xr_free(resolved_targs);
                return xr_type_new_error(NULL);
            }
            xa_check_span_generic_class_type_args(ctx, node, struct_name, resolved_targs,
                                                  sl->type_arg_count);
        }
    }

    // Infer field value types (for side effects / type checking), propagating
    // struct field types so nested literals lower to the declared layout.
    for (int i = 0; i < sl->field_count; i++) {
        XrType *saved_expected = ctx->expected_type;
        ctx->expected_type =
            class_info ? class_info_field_type(ctx, class_info, sl->field_names[i]) : NULL;
        xa_visit_infer_expr(ctx, sl->field_values[i]);
        ctx->expected_type = saved_expected;
    }

    if (class_sym && class_sym->kind == XA_SYM_CLASS) {
        if (links && links->class_info) {
            XrType *inst_type =
                resolved_targs ? xr_type_new_generic_instance(ctx->analyzer->isolate, struct_name,
                                                              links->class_info, resolved_targs,
                                                              sl->type_arg_count)
                               : xr_type_new_instance(ctx->analyzer->isolate, links->class_info);
            if (links->type && links->type->is_value_type) {
                inst_type->is_value_type = true;
            }
            if (resolved_targs && resolved_targs != resolved_targs_buf)
                xr_free(resolved_targs);
            return inst_type;
        }
        if (links && links->type) {
            if (resolved_targs && resolved_targs != resolved_targs_buf)
                xr_free(resolved_targs);
            return links->type;
        }
    }

    if (struct_name) {
        XrType *t = xr_type_new_class(ctx->analyzer->isolate, struct_name);
        t->is_value_type = true;
        if (resolved_targs && resolved_targs != resolved_targs_buf)
            xr_free(resolved_targs);
        return t;
    }
    if (resolved_targs && resolved_targs != resolved_targs_buf)
        xr_free(resolved_targs);
    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_ternary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    TernaryNode *tern = &node->as.ternary;
    // Visit condition to resolve variable symbol_ids
    XrType *cond_type = xa_visit_infer_expr(ctx, tern->condition);
    xa_check_condition_type(ctx, tern->condition, cond_type);
    // Bidirectional inference: propagate outer expected_type to both branches
    // (expected_type is already set by the caller, just pass through)
    XrType *then_type = xa_visit_infer_expr(ctx, tern->true_expr);
    XrType *else_type = xa_visit_infer_expr(ctx, tern->false_expr);

    if (xr_type_equals(then_type, else_type)) {
        return then_type;
    }
    return xr_type_union(ctx->analyzer->isolate, then_type, else_type);
}

/* ----------------------------------------------------------------------------
 * Nullish Coalesce: a ?? b
 * If a is T?, result is T | typeof(b). If a is T (non-nullable), result is T.
 * -------------------------------------------------------------------------- */
XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *left = xa_visit_infer_expr(ctx, node->as.binary.left);
    XrType *right = xa_visit_infer_expr(ctx, node->as.binary.right);

    // If left is nullable (T?), strip null and union with right
    // T? ?? U => T | U (most common: T? ?? T => T)
    XrType *non_null_left = xr_type_non_nullable(ctx->analyzer->isolate, left);
    if (non_null_left != left) {
        // left was nullable, result is non-null version unified with right
        if (xr_type_equals(non_null_left, right)) {
            return non_null_left;
        }
        return xr_type_union(ctx->analyzer->isolate, non_null_left, right);
    }

    // Left is not nullable, ?? is a no-op, return left type
    return left;
}

/* ----------------------------------------------------------------------------
 * Optional Chain: obj?.prop, obj?.[index], obj?.method(), func?.()
 * Result is always nullable: typeof(obj.prop) | null => T?
 * -------------------------------------------------------------------------- */
XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *obj_type = xa_visit_infer_expr(ctx, node->as.optional_chain.object);

    // If object is unknown, result is unknown
    if (XR_TYPE_IS_UNKNOWN(obj_type)) {
        return xr_type_new_unknown(NULL);
    }

    // Strip nullable from object for member lookup
    XrType *base_type = xr_type_non_nullable(ctx->analyzer->isolate, obj_type);

    // Optional function-call callee: func?.()
    if (node->as.optional_chain.chain_type == 3) {
        if (XR_TYPE_IS_FUNCTION(base_type))
            return xr_type_make_nullable(ctx->analyzer->isolate,
                                         xr_type_copy(ctx->analyzer->isolate, base_type));
        return xr_type_new_unknown(NULL);
    }

    // Property access: obj?.name
    if (node->as.optional_chain.name) {
        // Reuse member access logic by creating a temporary lookup
        // For now, handle common cases inline
        const char *prop_name = node->as.optional_chain.name;

        if (xa_freestanding_reject_string_member(ctx, node, base_type, prop_name))
            return xr_type_new_unknown(NULL);

        // Built-in properties — result is nullable (object may be null)
        if (xa_symbol_is_collection_length(xr_builtin_symbol_from_name(prop_name), base_type)) {
            return xr_type_make_nullable(ctx->analyzer->isolate, xr_type_new_int(NULL));
        }

        // Class instance member
        if (XR_TYPE_IS_INSTANCE(base_type) && base_type->instance.class_name) {
            XaSymbol *class_sym =
                xa_scope_lookup(ctx->analyzer->global_scope, base_type->instance.class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (class_links && class_links->class_info) {
                    XaSymbol *member =
                        xa_class_info_lookup_instance_member(class_links->class_info, prop_name);
                    if (member) {
                        XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                        if (ml && ml->type) {
                            XrType *result = xr_type_copy(ctx->analyzer->isolate, ml->type);
                            if (result)
                                result->is_nullable = true;
                            return result;
                        }
                    }
                }
            }
        }

        // Record/Json field access through optional chain: result is nullable
        // because the receiver may be null.
        if (XR_TYPE_HAS_OBJECT_SHAPE(base_type) && base_type->object.field_count > 0) {
            for (int i = 0; i < base_type->object.field_count; i++) {
                if (base_type->object.field_names[i] &&
                    strcmp(base_type->object.field_names[i], prop_name) == 0) {
                    XrType *result =
                        xr_type_copy(ctx->analyzer->isolate, base_type->object.field_types[i]);
                    if (result)
                        result->is_nullable = true;
                    return result;
                }
            }
        }

        // Built-in methods on primitive/container types (e.g. string?.toInt())
        if (xa_builtin_is_method(base_type, prop_name)) {
            const char *sig = xa_builtin_get_member_signature(base_type, prop_name);
            if (sig) {
                XrType *fn_type = xa_builtin_parse_full_signature(ctx->analyzer->isolate, sig);
                if (fn_type) {
                    XrType *result = xr_type_make_nullable(ctx->analyzer->isolate, fn_type);
                    return result;
                }
            }
        }

        // Built-in non-method properties (e.g. channel?.isClosed)
        {
            const char *sig = xa_builtin_get_member_signature(base_type, prop_name);
            if (sig && sig[0] == ':') {
                const char *type_str = sig + 1;
                while (*type_str == ' ')
                    type_str++;
                XrType *prop_type = xa_builtin_parse_type_string(ctx->analyzer->isolate, type_str);
                if (prop_type)
                    return xr_type_make_nullable(ctx->analyzer->isolate, prop_type);
            }
        }
    }

    // Index access: obj?.[index]
    if (node->as.optional_chain.index) {
        xa_visit_infer_expr(ctx, node->as.optional_chain.index);

        if ((XR_TYPE_IS_ARRAY(base_type) || XR_TYPE_IS_VIEW(base_type) ||
             XR_TYPE_IS_SPAN(base_type)) &&
            base_type->container.element_type) {
            XrType *result =
                xr_type_copy(ctx->analyzer->isolate, base_type->container.element_type);
            if (result)
                result->is_nullable = true;
            return result;
        }
        if (XR_TYPE_IS_MAP(base_type) && base_type->map.value_type) {
            XrType *result = xr_type_copy(ctx->analyzer->isolate, base_type->map.value_type);
            if (result)
                result->is_nullable = true;
            return result;
        }
    }

    // Fallback: any?
    return xr_type_new_unknown(NULL);
}

static bool xa_cast_type_is_uncertain(XrType *type) {
    if (!type)
        return true;
    if (XR_TYPE_IS_UNKNOWN_OR_ERROR(type) || XR_TYPE_IS_NEVER(type) ||
        XR_TYPE_IS_TYPE_PARAM(type) || XR_TYPE_IS_JSON(type))
        return true;
    return false;
}

static XrType *xa_cast_non_nullable_type(XrType *type) {
    if (type && type->is_nullable)
        return xr_type_non_nullable(NULL, type);
    return type;
}

static bool xa_cast_types_have_builtin_conversion(XrType *source, XrType *target) {
    XrType *source_base = xa_cast_non_nullable_type(source);
    XrType *target_base = xa_cast_non_nullable_type(target);
    if (!source_base || !target_base)
        return true;

    if (source_base->kind == XR_KIND_POINTER && target_base->kind == XR_KIND_POINTER) {
        return source_base->ptr_is_mut || !target_base->ptr_is_mut;
    }

    if (XR_TYPE_IS_NUMERIC(source_base)) {
        return XR_TYPE_IS_NUMERIC(target_base) || XR_TYPE_IS_STRING(target_base) ||
               XR_TYPE_IS_BOOL(target_base);
    }
    if ((XR_TYPE_IS_BOOL(source_base) || XR_TYPE_IS_RUNE(source_base)) &&
        XR_TYPE_IS_STRING(target_base))
        return true;
    if (XR_TYPE_IS_ENUM(source_base) && XR_TYPE_IS_INT(target_base))
        return true;
    return false;
}

static bool xa_cast_types_may_overlap(XrType *source, XrType *target) {
    if (xa_cast_type_is_uncertain(source) || xa_cast_type_is_uncertain(target))
        return true;
    if (XR_TYPE_IS_UNION(source)) {
        for (int i = 0; i < source->union_type.member_count; i++) {
            if (xa_cast_types_may_overlap(source->union_type.members[i], target))
                return true;
        }
        return false;
    }
    if (XR_TYPE_IS_UNION(target)) {
        for (int i = 0; i < target->union_type.member_count; i++) {
            if (xa_cast_types_may_overlap(source, target->union_type.members[i]))
                return true;
        }
        return false;
    }
    if (xa_cast_types_have_builtin_conversion(source, target))
        return true;
    if (xr_type_assignable(target, source))
        return true;
    if (xr_type_assignable(source, target))
        return true;
    return false;
}

/* as type cast: expr as T — returns T (non-safe), or T? (safe). */
XrType *xa_visit_as_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_error(NULL);
    // Visit operand to ensure it's analyzed (side effects, narrowing). `as`
    // is an explicit conversion, so do not use the target as assignment
    // context; casts such as `2147483648 as int32` intentionally truncate.
    XrType *source = xa_visit_infer_expr(ctx, node->as.as_expr.expr);
    XrType *target = node->as.as_expr.type
                         ? xr_tref_resolve_in_analyzer(ctx->analyzer, node->as.as_expr.type)
                         : NULL;
    if (!target)
        return xr_type_new_error(NULL);
    if (!xa_cast_types_may_overlap(source, target)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot cast type '%s' to unrelated type '%s'",
                 xr_type_to_string(source), xr_type_to_string(target));
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
    }
    if (xa_type_contains_span_view(source) && !xa_type_contains_span_view(target)) {
        xa_check_span_value_escape(ctx, node->as.as_expr.expr ? node->as.as_expr.expr : node,
                                   source, "erase Slice view with cast");
    }
    return target;
}

/* Force unwrap: expr! strips nullable from T? to produce T.
 * If operand is already non-nullable, the ! is a no-op (no warning for now).
 * If operand is null type, the ! is always a runtime panic. */
XrType *xa_visit_force_unwrap(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);
    XrType *inner = xa_visit_infer_expr(ctx, node->as.unary.operand);
    if (!inner)
        return xr_type_new_unknown(NULL);
    // Strip nullable: T? -> T
    if (inner->is_nullable) {
        return xr_type_non_nullable(ctx->analyzer->isolate, inner);
    }
    // Already non-nullable or any: return as-is
    return inner;
}

XrType *xa_visit_function_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_function(NULL, NULL, 0, xr_type_new_unknown(NULL), false);

    xa_freestanding_report_unavailable(ctx, node, "anonymous function/closure",
                                       "use a top-level function symbol");

    FunctionDeclNode *fn = &node->as.function_expr;
    const char **type_param_names = NULL;
    const char *type_param_buf[8];
    if (fn->type_param_count > 0 && fn->type_params) {
        type_param_names = (fn->type_param_count <= 8)
                               ? type_param_buf
                               : xr_malloc(sizeof(const char *) * fn->type_param_count);
        if (type_param_names) {
            for (int i = 0; i < fn->type_param_count; i++)
                type_param_names[i] = fn->type_params[i] ? fn->type_params[i]->name : NULL;
        }
    }

    // Check if has rest parameter
    bool has_rest = false;
    for (int i = 0; i < fn->param_count; i++) {
        if (fn->params[i] && fn->params[i]->is_rest) {
            has_rest = true;
            break;
        }
    }

    // Extract expected function type for bidirectional inference
    XrType *expected_fn = NULL;
    if (ctx->expected_type && XR_TYPE_IS_FUNCTION(ctx->expected_type)) {
        expected_fn = ctx->expected_type;
    }

    XrType **param_types = NULL;
    XrParamMode *param_modes = NULL;
    if (fn->param_count > 0) {
        param_types = xr_malloc(sizeof(XrType *) * fn->param_count);
        param_modes = xr_malloc(sizeof(XrParamMode) * fn->param_count);
        if (!param_types || !param_modes) {
            xr_free(param_types);
            xr_free(param_modes);
            if (type_param_names && type_param_names != type_param_buf)
                xr_free((void *) type_param_names);
            return xr_type_new_unknown(NULL);
        }
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *p = fn->params[i];
            XrParamMode mode = p ? p->passing_mode : XR_PARAM_VALUE;
            if (p && !p->type && mode == XR_PARAM_VALUE && expected_fn &&
                i < expected_fn->function.param_count) {
                mode = xr_type_function_param_mode(expected_fn, i);
            }
            param_modes[i] = mode;
            // Check for explicit type annotation first
            if (p && p->type) {
                param_types[i] = xr_tref_resolve_in_analyzer(ctx->analyzer, p->type);
                if (type_param_names) {
                    param_types[i] =
                        resolve_class_to_type_param(ctx->analyzer->isolate, param_types[i],
                                                    type_param_names, fn->type_param_count);
                }
            }
            // Use expected function type (bidirectional inference)
            else if (expected_fn && i < expected_fn->function.param_count &&
                     xr_type_function_param_type(expected_fn, i)) {
                param_types[i] = xr_type_function_param_type(expected_fn, i);
            }
            // Use generic inference from callback context
            else if (i == 0 && ctx->callback_accumulator_type) {
                param_types[i] = ctx->callback_accumulator_type;
            } else if (i == 0 && ctx->callback_element_type) {
                param_types[i] = ctx->callback_element_type;
            } else if (i == 1 && ctx->callback_accumulator_type && ctx->callback_element_type) {
                param_types[i] = ctx->callback_element_type;
            } else if (i == 1 && ctx->callback_index_type) {
                param_types[i] = ctx->callback_index_type;
            } else if (i == 2 && ctx->callback_accumulator_type && ctx->callback_index_type) {
                param_types[i] = ctx->callback_index_type;
            } else if (i == 2 && ctx->callback_array_type) {
                param_types[i] = ctx->callback_array_type;
            } else if (i == 3 && ctx->callback_array_type) {
                param_types[i] = ctx->callback_array_type;
            } else {
                // Cannot infer parameter type — report error
                param_types[i] = xr_type_new_unknown(NULL);
                if (p && p->name && !p->is_rest) {
                    XrLocation loc = {.file = ctx->file_path, .line = p->line, .column = p->column};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Parameter '%s' of anonymous function cannot be inferred, add "
                             "explicit type annotation",
                             p->name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                }
            }
        }
    }

    // Use expected return type if not explicitly declared
    XrType *return_type = fn->return_type
                              ? xr_tref_resolve_in_analyzer(ctx->analyzer, fn->return_type)
                              : xr_type_new_unknown(NULL);
    if (fn->return_type && type_param_names) {
        return_type = resolve_class_to_type_param(ctx->analyzer->isolate, return_type,
                                                  type_param_names, fn->type_param_count);
    }
    if (XR_TYPE_IS_UNKNOWN(return_type) && expected_fn && expected_fn->function.return_type) {
        return_type = expected_fn->function.return_type;
    }

    // Enter function scope, register params, and analyze body.
    // This is required even when return_type is already known (from
    // expected context) — the body visit resolves variable symbol_ids
    // and validates scope constraints.  Without it, captured variables
    // get symbol_id=0 and upvalue resolution fails.
    bool need_return_infer = XR_TYPE_IS_UNKNOWN(return_type);
    if (fn->body) {
        // Closure bodies execute lazily, so the definite-assignment check
        // produces false positives for variables captured from enclosing
        // scopes that are textually assigned after the closure literal.
        // We snapshot the diagnostics list before visiting and, after the
        // body visit, drop only the entries tagged USED_BEFORE_ASSIGN.
        // Real semantic errors (e.g. throw on a non-enum error, type
        // mismatches inside the body) are kept regardless of whether the
        // return type was already known.
        int saved_diag_count = ctx->analyzer->diagnostic_count;
        XaDiagnostic *saved_diag_tail = ctx->analyzer->diagnostics_tail;

        // Save outer function's return type collection state
        XrType **saved_return_types = ctx->return_types;
        int saved_return_count = ctx->return_type_count;
        int saved_return_cap = ctx->return_type_capacity;
        ctx->return_types = NULL;
        ctx->return_type_count = 0;
        ctx->return_type_capacity = 0;

        // Isolate flow graph for lambda body (same reason as named functions)
        XaFlowNode *saved_flow = NULL;
        XrFlowLabel *saved_break = NULL;
        XrFlowLabel *saved_continue = NULL;
        XrFlowLabel *saved_return_tgt = NULL;
        XrFlowLabel *saved_exception = NULL;
        if (ctx->flow) {
            saved_flow = ctx->flow->current_flow;
            saved_break = ctx->flow->current_break_target;
            saved_continue = ctx->flow->current_continue_target;
            saved_return_tgt = ctx->flow->current_return_target;
            saved_exception = ctx->flow->current_exception_target;
            xa_flow_create_start(ctx->flow);
            ctx->flow->current_break_target = NULL;
            ctx->flow->current_continue_target = NULL;
            ctx->flow->current_return_target = NULL;
            ctx->flow->current_exception_target = NULL;
        }

        const char *saved_pending_parallel_callback_name = ctx->pending_parallel_callback_name;
        bool saved_in_parallel_callback_body = ctx->in_parallel_callback_body;
        XaScope *saved_parallel_callback_scope = ctx->parallel_callback_scope;
        const char *saved_parallel_callback_name = ctx->parallel_callback_name;

        // Enter function scope
        xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);

        if (saved_pending_parallel_callback_name) {
            ctx->pending_parallel_callback_name = NULL;
            ctx->in_parallel_callback_body = true;
            ctx->parallel_callback_scope = ctx->analyzer->current_scope;
            ctx->parallel_callback_name = saved_pending_parallel_callback_name;
        } else if (saved_in_parallel_callback_body) {
            // Nested lambdas are not themselves the stdlib parallel callback
            // parameter. Do not apply the outer callback's capture contract to
            // their bodies as if they were lane bodies.
            ctx->in_parallel_callback_body = false;
            ctx->parallel_callback_scope = NULL;
            ctx->parallel_callback_name = NULL;
        }

        // Register parameters in scope (with their inferred types)
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *p = fn->params[i];
            if (p && p->name) {
                XaSymbol *param_sym = xa_symbol_new(p->name, XA_SYM_PARAMETER);
                param_sym->location.line = p->line > 0 ? p->line : node->line;
                xa_visit_add_symbol_checked(ctx, param_sym, 0);
                p->symbol_id = param_sym->id;
                XaSymbolLinks *pl = xa_analyzer_get_links(ctx->analyzer, param_sym);
                pl->type = param_types ? param_types[i] : xr_type_new_unknown(NULL);
                param_sym->passing_mode = param_modes ? param_modes[i] : p->passing_mode;
                pl->is_definitely_assigned = param_sym->passing_mode != XR_PARAM_OUT;
            }
        }

        // Set expected return type for bidirectional inference on return
        // stmts.  Priority order:
        //   1. user-written annotation on the closure (`fn(d) -> int { ... }`),
        //      which is authoritative for what the body must return;
        //   2. an outer contextual type that pins down a concrete result
        //      (e.g. assigning the closure to a typed variable);
        //   3. otherwise NULL so the enclosing function's
        //      expected_return_type doesn't leak in (e.g. `void` from a
        //      parent fn would spuriously fail value-returning closure
        //      bodies).
        // Skipping unbound type parameters (`U` inside `map<T,U>(callback)`)
        // is critical: the analyzer's monomorphisation can't always resolve
        // those at the call site, and using `U` as expected_return_type
        // produces false "expected 'U', got '<concrete>'" errors on
        // perfectly valid closures.
        XrType *saved_expected_ret = ctx->expected_return_type;
        if (fn->return_type) {
            ctx->expected_return_type = return_type;
        } else if (expected_fn && expected_fn->function.return_type &&
                   !XR_TYPE_IS_UNKNOWN(expected_fn->function.return_type) &&
                   expected_fn->function.return_type->kind != XR_KIND_TYPE_PARAM) {
            ctx->expected_return_type = expected_fn->function.return_type;
        } else {
            ctx->expected_return_type = NULL;
        }

        // Unified body visitor: idempotent collect + direct traversal.
        // A variable is hidden from its own initializer, but nested closure
        // bodies are analyzed after the binding exists and may capture it
        // (e.g. `var f = fn() { return f() }`).
        uint32_t saved_initializing_symbol_id = ctx->initializing_symbol_id;
        ctx->initializing_symbol_id = 0;
        xa_visit_function_body_unified(ctx, fn->body);
        xa_check_out_params_assigned_at_function_exit(ctx, ctx->analyzer->current_scope, fn->body);
        xa_parallel_callback_effect_check(ctx, fn->body);
        ctx->initializing_symbol_id = saved_initializing_symbol_id;

        ctx->expected_return_type = saved_expected_ret;

        // Compute unified return type from all collected return statements
        if (need_return_infer) {
            XrType *inferred_ret = xa_infer_compute_return_type(ctx);
            if (inferred_ret && !XR_TYPE_IS_UNKNOWN(inferred_ret)) {
                return_type = inferred_ret;
            }
        }

        xa_analyzer_exit_scope(ctx->analyzer);

        ctx->pending_parallel_callback_name = saved_pending_parallel_callback_name;
        ctx->in_parallel_callback_body = saved_in_parallel_callback_body;
        ctx->parallel_callback_scope = saved_parallel_callback_scope;
        ctx->parallel_callback_name = saved_parallel_callback_name;

        // Drop only USED_BEFORE_ASSIGN false positives from the body visit;
        // keep every other diagnostic so genuine errors inside anonymous
        // functions surface to the user.
        if (ctx->analyzer->diagnostic_count > saved_diag_count) {
            XaDiagnostic **link =
                saved_diag_tail ? &saved_diag_tail->next : &ctx->analyzer->diagnostics;
            XaDiagnostic *d = *link;
            XaDiagnostic *new_tail = saved_diag_tail;
            int kept = 0;
            while (d) {
                XaDiagnostic *next = d->next;
                if (d->code == XR_ERR_ANALYZE_USED_BEFORE_ASSIGN && d->message &&
                    strncmp(d->message, "Variable '", strlen("Variable '")) == 0) {
                    if (d->message)
                        xr_free((void *) d->message);
                    xr_free(d);
                    *link = next;
                } else {
                    new_tail = d;
                    link = &d->next;
                    kept++;
                }
                d = next;
            }
            ctx->analyzer->diagnostics_tail = new_tail;
            ctx->analyzer->diagnostic_count = saved_diag_count + kept;
        }

        // Restore flow state to enclosing function's context
        if (ctx->flow) {
            ctx->flow->current_flow = saved_flow;
            ctx->flow->current_break_target = saved_break;
            ctx->flow->current_continue_target = saved_continue;
            ctx->flow->current_return_target = saved_return_tgt;
            ctx->flow->current_exception_target = saved_exception;
        }

        // Restore outer function's return type state
        if (ctx->return_types)
            xr_free(ctx->return_types);
        ctx->return_types = saved_return_types;
        ctx->return_type_count = saved_return_count;
        ctx->return_type_capacity = saved_return_cap;
    }

    // After full body analysis, report error if return type still unknown
    if (XR_TYPE_IS_UNKNOWN(return_type) && fn->body && xa_body_has_return_expr(fn->body)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE,
            "Anonymous function returns a value but return type cannot be inferred, "
            "add explicit type annotation",
            &loc);
    }

    XrType *result = xr_type_new_function(ctx->analyzer->isolate, param_types, fn->param_count,
                                          return_type, has_rest);
    if (result) {
        result->function.min_params = fn->required_count;
        for (int i = 0; i < fn->param_count; i++) {
            if (fn->params[i])
                xr_type_function_set_param_mode(
                    result, i, param_modes ? param_modes[i] : fn->params[i]->passing_mode);
        }
    }
    xa_set_function_type_params_from_ast(ctx, result, fn->type_params, fn->type_param_count);

    if (param_types)
        xr_free(param_types);
    if (param_modes)
        xr_free(param_modes);
    if (type_param_names && type_param_names != type_param_buf)
        xr_free((void *) type_param_names);
    return result;
}

/* ----------------------------------------------------------------------------
 * Cross-Coroutine Boundary Transfer Validation
 *
 * Heap-shaped values that currently need a coroutine-boundary clone must be
 * transferred with explicit syntax. This keeps the source model aligned with
 * AOT cost: copy(...) means the user accepts O(n), move means ownership leaves
 * the current coroutine, and shared is the zero-copy read-only path.
 * -------------------------------------------------------------------------- */
bool xa_boundary_transfer_type_needs_explicit(const XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type) || XR_TYPE_IS_NEVER(type) || XR_TYPE_IS_NULL(type))
        return false;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_VIEW:
        case XR_KIND_SPAN:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_JSON:
        case XR_KIND_RECORD:
            return true;
        case XR_KIND_INSTANCE:
            return type->instance.class_name &&
                   strcmp(type->instance.class_name, "StringBuilder") == 0;
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (xa_boundary_transfer_type_needs_explicit(type->union_type.members[i]))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

bool xa_boundary_arg_is_explicit_copy(AstNode *arg_node) {
    if (!arg_node || arg_node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &arg_node->as.call_expr;
    if (call->arg_count != 1 || !call->callee || call->callee->type != AST_VARIABLE)
        return false;
    const char *name = call->callee->as.variable.name;
    return name && strcmp(name, "copy") == 0;
}

bool xa_boundary_arg_is_shared(XaInferContext *ctx, AstNode *arg_node) {
    if (!ctx || !ctx->analyzer || !arg_node || arg_node->type != AST_VARIABLE)
        return false;
    const char *name = arg_node->as.variable.name;
    XaSymbol *sym = name ? xa_scope_lookup(ctx->analyzer->current_scope, name) : NULL;
    return xa_symbol_has_shared_provenance(sym);
}

XaSymbol *xa_boundary_move_source_symbol(XaInferContext *ctx, AstNode *arg_node) {
    if (!ctx || !ctx->analyzer || !arg_node || arg_node->type != AST_MOVE_EXPR)
        return NULL;
    AstNode *inner = arg_node->as.move_expr.expr;
    if (!inner || inner->type != AST_VARIABLE || !inner->as.variable.name)
        return NULL;
    return xa_scope_lookup(ctx->analyzer->current_scope, inner->as.variable.name);
}

static bool xa_boundary_arg_is_owned_move(XaInferContext *ctx, AstNode *arg_node) {
    XaSymbol *sym = xa_boundary_move_source_symbol(ctx, arg_node);
    return sym && sym->is_owned;
}

static void xa_report_boundary_local_move(XaInferContext *ctx, AstNode *boundary_node,
                                          AstNode *arg_node, const char *boundary_label) {
    if (!ctx || !ctx->analyzer || !arg_node)
        return;
    AstNode *inner = arg_node->type == AST_MOVE_EXPR ? arg_node->as.move_expr.expr : NULL;
    const char *name = inner && inner->type == AST_VARIABLE && inner->as.variable.name
                           ? inner->as.variable.name
                           : "?";
    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node->line ? arg_node->line
                                             : (boundary_node ? boundary_node->line : 0),
                      .column = arg_node->column ? arg_node->column
                                                 : (boundary_node ? boundary_node->column : 0)};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "%s move of '%s' requires an owned source; use copy(%s) for local storage or "
             "declare '%s' with owned",
             boundary_label ? boundary_label : "cross-coroutine value", name, name, name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

void xa_check_boundary_transfer_arg(XaInferContext *ctx, AstNode *boundary_node, AstNode *arg_node,
                                    XrType *arg_type, const char *boundary_label) {
    if (!ctx || !ctx->analyzer || !arg_node)
        return;
    if (xa_type_contains_span_view(arg_type)) {
        char context[160];
        snprintf(context, sizeof(context), "send Slice view across %s",
                 boundary_label ? boundary_label : "coroutine boundary");
        xa_check_span_value_escape(ctx, arg_node, arg_type, context);
        return;
    }
    if (!xa_boundary_transfer_type_needs_explicit(arg_type))
        return;
    if (arg_node->type == AST_MOVE_EXPR) {
        if (xa_boundary_arg_is_owned_move(ctx, arg_node))
            return;
        xa_report_boundary_local_move(ctx, boundary_node, arg_node, boundary_label);
        return;
    }
    if (xa_boundary_arg_is_explicit_copy(arg_node) || xa_boundary_arg_is_shared(ctx, arg_node))
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = arg_node->line ? arg_node->line
                                             : (boundary_node ? boundary_node->line : 0),
                      .column = arg_node->column ? arg_node->column
                                                 : (boundary_node ? boundary_node->column : 0)};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "%s transfer of type '%s' requires explicit copy(...) or move of an owned source; "
             "use shared for shared identity",
             boundary_label ? boundary_label : "cross-coroutine value",
             xr_type_to_string(arg_type));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                               &loc);
}

static void xa_check_go_call_boundary_args(XaInferContext *ctx, AstNode *go_node,
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
                    xa_check_boundary_transfer_arg(ctx, go_node, arg, src->tuple.element_types[j],
                                                   "go argument");
                }
            }
            continue;
        }
        XrType *arg_type = xa_analyzer_get_node_type(ctx->analyzer, arg);
        if (!arg_type)
            arg_type = xa_visit_infer_expr(ctx, arg);
        xa_check_boundary_transfer_arg(ctx, go_node, arg, arg_type, "go argument");
    }
}

XrType *xa_visit_go_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_task(ctx->analyzer->isolate, xr_type_new_unknown(NULL));

    GoExprNode *go = &node->as.go_expr;
    bool is_thread_spawn = go->spawn_kind == XR_SPAWN_THREAD;

    // Infer the type of the expression being spawned
    XrType *result_type = xr_type_new_unit(NULL);
    if (go->expr) {
        XrType *expr_type = xa_visit_infer_expr(ctx, go->expr);
        if (go->expr->type == AST_CALL_EXPR)
            xa_check_go_call_boundary_args(ctx, node, &go->expr->as.call_expr);
        // If spawning a function call, get its return type
        if (XR_TYPE_IS_FUNCTION(expr_type) && expr_type->function.return_type) {
            result_type = expr_type->function.return_type;
        } else if (!XR_TYPE_IS_FUNCTION(expr_type)) {
            // Direct expression result
            result_type = expr_type;
        }
    }

    if (is_thread_spawn) {
        XrType **args = (XrType **) xr_malloc(sizeof(XrType *));
        if (!args)
            return xr_type_new_named_instance(ctx->analyzer->isolate, "Thread");
        args[0] = result_type ? result_type : xr_type_new_unknown(NULL);
        return xr_type_new_generic_instance(ctx->analyzer->isolate, "Thread", NULL, args, 1);
    }

    // go expr returns Task<T> where T is the result type
    return xr_type_new_task(ctx->analyzer->isolate, result_type);
}

XrType *xa_visit_await_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    AwaitExprNode *await = &node->as.await_expr;

    // Infer the type of the awaited expression
    if (await->expr) {
        XrType *expr_type = xa_visit_infer_expr(ctx, await->expr);

        // await all/any/anySuccess operates on Array<Task<T>> → Array<T> / T
        if (await->is_all || await->is_any || await->is_any_success) {
            // These forms take an array of tasks; extract element type
            if (expr_type && XR_TYPE_IS_ARRAY(expr_type)) {
                XrType *elem = expr_type->container.element_type;
                XrType *result_elem = xr_type_new_unknown(NULL);
                if (xr_type_is_named_class(elem, "Task") && elem->instance.type_arg_count > 0) {
                    result_elem = elem->instance.type_args[0];
                }
                if (await->is_all) {
                    if (await->into) {
                        XrType *into_type = xa_visit_infer_expr(ctx, await->into);
                        if (into_type && !XR_TYPE_IS_UNKNOWN(into_type)) {
                            if (!XR_TYPE_IS_ARRAY(into_type)) {
                                XrLocation loc = {.file = ctx->file_path,
                                                  .line = await->into->line,
                                                  .column = await->into->column};
                                xa_analyzer_add_diagnostic(
                                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE,
                                    "await all into expects an Array result buffer", &loc);
                            } else if (result_elem && !XR_TYPE_IS_UNKNOWN(result_elem) &&
                                       into_type->container.element_type &&
                                       !xr_type_equals(into_type->container.element_type,
                                                       result_elem)) {
                                XrLocation loc = {.file = ctx->file_path,
                                                  .line = await->into->line,
                                                  .column = await->into->column};
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                         "await all into result buffer type mismatch: expected "
                                         "Array<%s>, got %s",
                                         xr_type_to_string(result_elem),
                                         xr_type_to_string(into_type));
                                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                           XR_ERR_ANALYZE_AWAIT_TYPE, msg, &loc);
                            }
                        }
                        return xr_type_new_unit(ctx->analyzer->isolate);
                    }
                    return xr_type_new_array(ctx->analyzer->isolate, result_elem);
                }
                if (await->into) {
                    XrLocation loc = {.file = ctx->file_path,
                                      .line = await->into->line,
                                      .column = await->into->column};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_AWAIT_TYPE,
                                               "await into is only supported for await all", &loc);
                }
                // await any / anySuccess returns single element
                return result_elem;
            }
            return xr_type_new_unknown(NULL);
        }

        // Single await: extract result type from Task<T>
        // Failed/cancelled tasks propagate via exception, not null.
        if (xr_type_is_named_class(expr_type, "Task")) {
            XrType *result_type =
                (expr_type->instance.type_arg_count > 0) ? expr_type->instance.type_args[0] : NULL;
            if (!result_type)
                return xr_type_new_unknown(NULL);
            return result_type;
        }

        // await [arr] is syntactic sugar for await all — treat array as Task array
        if (XR_TYPE_IS_ARRAY(expr_type)) {
            XrType *elem = expr_type->container.element_type;
            XrType *result_elem = xr_type_new_unknown(NULL);
            if (elem && xr_type_is_named_class(elem, "Task") && elem->instance.type_arg_count > 0) {
                result_elem = elem->instance.type_args[0];
            }
            return xr_type_new_array(ctx->analyzer->isolate, result_elem);
        }

        // If not a Task, report error (skip for unknown type which means inference failed)
        if (expr_type && !XR_TYPE_IS_UNKNOWN(expr_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE,
                                       "await expects a Task type", &loc);
        }
    }

    return xr_type_new_unknown(NULL);
}

/*
 * Visit unsafe expression: unsafe { operand }
 *
 * Transparent: the value and type are those of the operand. The wrapper
 * raises ctx->unsafe_depth for the operand's analysis so that extern calls
 * and raw-pointer dereference inside are permitted (and rejected outside).
 */
XrType *xa_visit_unsafe_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);
    AstNode *body = node->as.unsafe_expr.operand;
    if (!body)
        return xr_type_new_unit(ctx->analyzer->isolate);

    ctx->unsafe_depth++;
    XrType *result = NULL;
    if (body->type == AST_BLOCK) {
        /* Statement block: visit each statement; the trailing expression
         * statement (if any) yields the unsafe expression's value. */
        BlockNode *blk = &body->as.block;
        for (int i = 0; i < blk->count; i++) {
            AstNode *stmt = blk->statements[i];
            if (!stmt)
                continue;
            bool is_last = (i == blk->count - 1);
            if (is_last && stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
                AstNode *inner = stmt->as.expr_stmt;
                if (inner && (inner->type == AST_MEMBER_SET || inner->type == AST_ASSIGNMENT ||
                              inner->type == AST_COMPOUND_ASSIGNMENT || inner->type == AST_INC ||
                              inner->type == AST_DEC || inner->type == AST_INDEX_SET)) {
                    xa_visit_infer_stmt(ctx, inner);
                    result = xr_type_new_unit(ctx->analyzer->isolate);
                } else {
                    result = xa_visit_infer_expr(ctx, inner);
                }
            } else {
                xa_visit_infer_stmt(ctx, stmt);
            }
        }
    } else {
        /* Defensive: a non-block body is a single expression. */
        result = xa_visit_infer_expr(ctx, body);
    }
    ctx->unsafe_depth--;
    return result ? result : xr_type_new_unit(ctx->analyzer->isolate);
}

/*
 * Visit move expression: move var
 * Compile-time checks:
 *   - must be a variable (enforced by parser)
 *   - cannot move const value
 *   - cannot move Channel (thread-safe, shared by incref)
 *   - cannot move value types (int/float/bool/string — no heap object)
 */
XrType *xa_visit_move_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    MoveExprNode *move = &node->as.move_expr;
    AstNode *inner = move->expr;
    if (!inner)
        return xr_type_new_unknown(NULL);

    XaSymbol *move_sym = NULL;
    if (inner->type == AST_VARIABLE) {
        move_sym = xa_scope_lookup(ctx->analyzer->current_scope, inner->as.variable.name);
    }

    // Infer type of the variable being moved
    const AstNode *saved_move_source_node = ctx->current_move_source_node;
    uint32_t saved_move_source_symbol_id = ctx->current_move_source_symbol_id;
    bool saved_move_source_allows_stale_mark = ctx->current_move_source_allows_stale_mark;
    ctx->current_move_source_node = NULL;
    ctx->current_move_source_symbol_id = 0;
    ctx->current_move_source_allows_stale_mark = false;
    if (move_sym) {
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, move_sym);
        ctx->current_move_source_node = inner;
        ctx->current_move_source_symbol_id = move_sym->id;
        ctx->current_move_source_allows_stale_mark = links && links->move_state == XA_MOVE_MOVED &&
                                                     links->moved_line == (uint32_t) node->line &&
                                                     links->moved_column == (uint32_t) node->column;
    }
    XrType *var_type = xa_visit_infer_expr(ctx, inner);
    ctx->current_move_source_node = saved_move_source_node;
    ctx->current_move_source_symbol_id = saved_move_source_symbol_id;
    ctx->current_move_source_allows_stale_mark = saved_move_source_allows_stale_mark;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};

    // Check: variable must exist and be a movable var binding.
    if (move_sym && (move_sym->is_const || move_sym->is_shared)) {
        const char *name = inner->as.variable.name;
        char msg[128];
        snprintf(msg, sizeof(msg),
                 move_sym->is_shared ? "cannot move shared binding '%s'"
                                     : "cannot move const value '%s'",
                 name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }
    xa_check_active_span_borrow_owner_mutation(ctx, node, move_sym, "moving the owner");
    // Mark the variable as moved — subsequent accesses are compile errors
    if (move_sym) {
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, move_sym);
        if (links) {
            links->move_state = XA_MOVE_MOVED;
            links->moved_line = (uint32_t) node->line;
            links->moved_column = (uint32_t) node->column;
        }
    }

    // Check: cannot move synchronization/concurrency handles.
    const char *handle_label = xa_concurrency_handle_label(var_type);
    if (handle_label) {
        char msg[128];
        snprintf(msg, sizeof(msg), "cannot move %s (shared concurrency handle)", handle_label);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }

    // Check: cannot move value types (no heap object to transfer)
    if (var_type && xr_kind_is_primitive(var_type->kind)) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "move is not meaningful for value type", &loc);
    }

    // move expr has the same type as the inner expression
    return var_type ? var_type : xr_type_new_unknown(NULL);
}
