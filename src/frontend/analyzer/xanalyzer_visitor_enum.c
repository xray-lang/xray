/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_enum.c - Named enum payload construction analysis
 */

#include "xanalyzer_visitor_internal.h"
#include "xa_enum_record_plan.h"
#include "xa_selection.h"
#include "xaddressability.h"

static void report_enum_record_error(XaInferContext *ctx, AstNode *node, int code,
                                     const char *message) {
    XrLocation location = {.file = ctx->file_path,
                           .line = node ? node->line : 0,
                           .column = node ? node->column : 0};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, message, &location);
}

static XaSymbol *enum_record_path_symbol(XaInferContext *ctx, AstNode *path,
                                         uint16_t *variant_ordinal) {
    if (!ctx || !ctx->analyzer || !path || !variant_ordinal)
        return NULL;

    const XaSelection *selection = xa_analyzer_get_selection(ctx->analyzer, path);
    if (selection && selection->kind == XA_SEL_ENUM_MEMBER && selection->target_symbol &&
        selection->target_symbol->kind == XA_SYM_ENUM && selection->field_index >= 0 &&
        selection->field_index <= UINT16_MAX) {
        *variant_ordinal = (uint16_t) selection->field_index;
        return selection->target_symbol;
    }

    const char *enum_name = NULL;
    const char *variant_name = NULL;
    if (path->type == AST_ENUM_ACCESS) {
        enum_name = path->as.enum_access.enum_name;
        variant_name = path->as.enum_access.member_name;
    } else if (path->type == AST_MEMBER_ACCESS && path->as.member_access.object &&
               path->as.member_access.object->type == AST_VARIABLE) {
        enum_name = path->as.member_access.object->as.variable.name;
        variant_name = path->as.member_access.name;
    }
    if (!enum_name || !variant_name)
        return NULL;

    XaSymbol *symbol = xa_lookup_visible_symbol(ctx, enum_name);
    if (!symbol || symbol->kind != XA_SYM_ENUM)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    int ordinal = xa_enum_info_find_variant(links ? links->enum_info : NULL, variant_name);
    if (ordinal < 0 || ordinal > UINT16_MAX)
        return NULL;
    *variant_ordinal = (uint16_t) ordinal;
    return symbol;
}

static int enum_payload_slot(const XaEnumVariantInfo *variant, const char *field_name) {
    if (!variant || !field_name || !variant->payload_names)
        return -1;
    for (uint16_t i = 0; i < variant->payload_count; i++) {
        if (variant->payload_names[i] && strcmp(variant->payload_names[i], field_name) == 0)
            return (int) i;
    }
    return -1;
}

static void check_enum_payload_value(XaInferContext *ctx, AstNode *construct, AstNode *value,
                                     XrType *payload_type, const char *enum_name,
                                     const char *variant_name, const char *field_name) {
    XrType *saved_expected = ctx->expected_type;
    XrType *saved_from_signature = ctx->expected_from_signature;
    if (payload_type && !XR_TYPE_IS_UNKNOWN(payload_type))
        ctx->expected_type = payload_type;
    ctx->expected_from_signature = ctx->expected_type;
    XrType *value_type = xa_visit_infer_expr(ctx, value);
    ctx->expected_type = saved_expected;
    ctx->expected_from_signature = saved_from_signature;

    char context[192];
    if (xa_type_contains_span_view(value_type)) {
        snprintf(context, sizeof(context), "store Slice view in enum payload '%s.%s.%s'",
                 enum_name, variant_name, field_name);
        xa_check_span_value_escape(ctx, value, value_type, context);
    }
    if (value_type && XR_TYPE_IS_POINTER(value_type)) {
        snprintf(context, sizeof(context), "store raw pointer borrow in enum payload '%s.%s.%s'",
                 enum_name, variant_name, field_name);
        xa_check_pointer_borrow_escape(ctx, value, value, value_type, context);
    }
    xa_note_owner_escapes_into_heap(ctx, value);

    if (!payload_type || XR_TYPE_IS_UNKNOWN(payload_type) || !value_type ||
        XR_TYPE_IS_UNKNOWN(value_type))
        return;
    XrLocation location = {
        .file = ctx->file_path, .line = value->line, .column = value->column};
    bool null_error =
        xa_check_null_safety(ctx->analyzer, payload_type, value_type, "Enum field", &location);
    if (!null_error && !xa_typecheck_assignable(payload_type, value_type) &&
        !xr_is_json_coercion(payload_type, value_type)) {
        char message[320];
        snprintf(message, sizeof(message),
                 "enum field '%s.%s.%s' has type '%s', but value has type '%s'", enum_name,
                 variant_name, field_name, xr_type_to_string(payload_type),
                 xr_type_to_string(value_type));
        report_enum_record_error(ctx, value, XR_ERR_ANALYZE_ARG_TYPE, message);
    }
    (void) construct;
}

XrType *xa_visit_enum_construct(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node || node->type != AST_ENUM_CONSTRUCT)
        return xr_type_new_error(ctx && ctx->analyzer ? ctx->analyzer->isolate : NULL);

    EnumConstructNode *construct = &node->as.enum_construct;
    bool saved_record_path = ctx->allow_payload_enum_record_path;
    ctx->allow_payload_enum_record_path = true;
    XrType *enum_type = xa_visit_infer_expr(ctx, construct->variant_path);
    ctx->allow_payload_enum_record_path = saved_record_path;

    uint16_t variant_ordinal = 0;
    XaSymbol *enum_symbol =
        enum_record_path_symbol(ctx, construct->variant_path, &variant_ordinal);
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, enum_symbol);
    XaEnumInfo *info = links ? links->enum_info : NULL;
    if (!enum_symbol || !info || !info->variants || variant_ordinal >= info->variant_count) {
        report_enum_record_error(ctx, node, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                 "named enum construction requires an exact enum variant path");
        for (int i = 0; i < construct->field_count; i++)
            xa_visit_infer_expr(ctx, construct->field_values[i]);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    XaEnumVariantInfo *variant = &info->variants[variant_ordinal];
    const char *enum_name = enum_symbol->name ? enum_symbol->name : "?";
    const char *variant_name = variant->name ? variant->name : "?";
    if (variant->payload_count == 0) {
        char message[224];
        snprintf(message, sizeof(message),
                 "unit enum variant '%s.%s' has no record payload and must be used without '{}'",
                 enum_name, variant_name);
        report_enum_record_error(ctx, node, XR_ERR_ANALYZE_WRONG_ARG_COUNT, message);
        for (int i = 0; i < construct->field_count; i++)
            xa_visit_infer_expr(ctx, construct->field_values[i]);
        return xr_type_new_error(ctx->analyzer->isolate);
    }

    uint16_t source_count = construct->field_count > UINT16_MAX
                                ? UINT16_MAX
                                : (uint16_t) construct->field_count;
    uint16_t *source_to_slot =
        source_count > 0 ? xr_malloc((size_t) source_count * sizeof(*source_to_slot)) : NULL;
    bool *seen = xr_calloc((size_t) variant->payload_count, sizeof(*seen));
    bool complete = source_count == (uint16_t) construct->field_count && seen &&
                    (source_count == 0 || source_to_slot);

    for (int i = 0; i < construct->field_count; i++) {
        const char *field_name = construct->field_names ? construct->field_names[i] : NULL;
        AstNode *value = construct->field_values ? construct->field_values[i] : NULL;
        int slot = enum_payload_slot(variant, field_name);
        if (slot < 0) {
            char message[256];
            snprintf(message, sizeof(message), "enum variant '%s.%s' has no payload field '%s'",
                     enum_name, variant_name, field_name ? field_name : "<missing>");
            report_enum_record_error(ctx, value ? value : node, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                     message);
            complete = false;
            if (value)
                xa_visit_infer_expr(ctx, value);
            continue;
        }
        if (seen && seen[slot]) {
            char message[256];
            snprintf(message, sizeof(message),
                     "enum payload field '%s.%s.%s' is initialized more than once", enum_name,
                     variant_name, field_name);
            report_enum_record_error(ctx, value ? value : node, XR_ERR_ANALYZE_ARG_TYPE, message);
            complete = false;
        } else if (seen) {
            seen[slot] = true;
        }
        if (source_to_slot && i < source_count)
            source_to_slot[i] = (uint16_t) slot;
        XrType *payload_type = xa_analyzer_resolve_adt_payload_type(
            ctx->analyzer, enum_type, construct->variant_path, slot);
        if (!payload_type && variant->payload_types)
            payload_type = variant->payload_types[slot];
        if (value)
            check_enum_payload_value(ctx, node, value, payload_type, enum_name, variant_name,
                                     field_name);
    }

    for (uint16_t slot = 0; slot < variant->payload_count; slot++) {
        if (seen && seen[slot])
            continue;
        char message[256];
        snprintf(message, sizeof(message), "enum construction '%s.%s' is missing field '%s'",
                 enum_name, variant_name,
                 variant->payload_names ? variant->payload_names[slot] : "<unnamed>");
        report_enum_record_error(ctx, node, XR_ERR_ANALYZE_WRONG_ARG_COUNT, message);
        complete = false;
    }

    if (complete) {
        XaEnumRecordPlan plan = {
            .kind = XA_ENUM_RECORD_CONSTRUCT,
            .enum_symbol_id = enum_symbol->id,
            .enum_layout_id = info->layout ? info->layout->layout_id : 0,
            .variant_ordinal = variant_ordinal,
            .source_field_count = source_count,
            .declaration_field_count = variant->payload_count,
            .source_to_slot = source_to_slot,
            .complete = true,
        };
        if (!xa_enum_record_plan_table_set(
                (XaEnumRecordPlanTable *) ctx->analyzer->enum_record_plan_table, node, &plan)) {
            report_enum_record_error(ctx, node, XR_ERR_ANALYZE,
                                     "failed to publish immutable enum construction plan");
        }
    }
    xr_free(seen);
    xr_free(source_to_slot);
    return enum_type ? enum_type : xr_type_new_error(ctx->analyzer->isolate);
}
