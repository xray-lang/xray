/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_pattern.c - Pass 2 visitor for match expressions
 *
 * KEY CONCEPT:
 *   `match (expr) { pattern => body, ... }` is the analyzer's most
 *   complex expression: it must (a) infer the union of all arm body
 *   types, (b) thread variable bindings through arm scopes, and
 *   (c) verify exhaustiveness for both enum subjects and `typeof()`
 *   subjects on union / nullable types. The supporting helpers
 *   (type_member_to_kind, kind_to_type_member, collect_matched_*
 *   pattern walkers) live alongside the visitor that uses them.
 *
 *   This file holds the match-expression / pattern-walker subset of
 *   the analyzer visitor.
 */

#include "xanalyzer_visitor_internal.h"
#include "xa_selection.h"
#include "xtype_ref_resolve.h"
#include "../../base/xchecks.h"

// Forward declaration (defined below in narrowing section)
// Defined in xanalyzer_visitor_stmt.c

// Helper: map Type.xxx member name to XrTypeKind for typeof exhaustiveness
static XrTypeKind type_member_to_kind(const char *name) {
    if (!name)
        return XR_KIND_COUNT;
    const XrExactScalarDesc *scalar = xr_exact_scalar_by_source_name(name, strlen(name));
    if (scalar)
        return scalar->family == XR_EXACT_SCALAR_FAMILY_INTEGER ? XR_KIND_INT : XR_KIND_FLOAT;
    if (strcmp(name, "string") == 0)
        return XR_KIND_STRING;
    if (strcmp(name, "bool") == 0)
        return XR_KIND_BOOL;
    if (strcmp(name, "rune") == 0)
        return XR_KIND_RUNE;
    if (strcmp(name, "null") == 0)
        return XR_KIND_NULL;
    if (strcmp(name, "Array") == 0)
        return XR_KIND_ARRAY;
    if (strcmp(name, "Map") == 0)
        return XR_KIND_MAP;
    if (strcmp(name, "Set") == 0)
        return XR_KIND_SET;
    if (strcmp(name, "object") == 0)
        return XR_KIND_STRUCT_OBJECT;
    if (strcmp(name, "function") == 0)
        return XR_KIND_FUNCTION;
    if (strcmp(name, "Regex") == 0)
        return XR_KIND_INSTANCE;
    if (strcmp(name, "BigInt") == 0)
        return XR_KIND_INSTANCE;
    if (strcmp(name, "Channel") == 0)
        return XR_KIND_CHANNEL;
    if (strcmp(name, "Atomic") == 0)
        return XR_KIND_INSTANCE;
    return XR_KIND_COUNT;
}

// Helper: get display name for XrTypeKind as Type.xxx
static const char *kind_to_type_member(XrTypeKind kind) {
    switch (kind) {
        case XR_KIND_INT:
            return "Type.i64";
        case XR_KIND_FLOAT:
            return "Type.f64";
        case XR_KIND_STRING:
            return "Type.string";
        case XR_KIND_BOOL:
            return "Type.bool";
        case XR_KIND_RUNE:
            return "Type.char";
        case XR_KIND_NULL:
            return "Type.null";
        case XR_KIND_ARRAY:
            return "Type.Array";
        case XR_KIND_MAP:
            return "Type.Map";
        case XR_KIND_SET:
            return "Type.Set";
        case XR_KIND_JSON:
        case XR_KIND_STRUCT_OBJECT:
            return "Type.object";
        case XR_KIND_FUNCTION:
            return "Type.function";
        case XR_KIND_CHANNEL:
            return "Type.Channel";
        default:
            return NULL;
    }
}

// Helper: collect Type.xxx member names from a match arm pattern
static void collect_matched_type_members(AstNode *pattern, XrTypeKind *kinds, int *count, int cap) {
    if (!pattern || *count >= cap)
        return;

    if (pattern->type == AST_PATTERN_LITERAL && pattern->as.pattern_literal.value) {
        AstNode *val = pattern->as.pattern_literal.value;
        if (val->type == AST_MEMBER_ACCESS && val->as.member_access.object &&
            val->as.member_access.object->type == AST_VARIABLE &&
            val->as.member_access.object->as.variable.name &&
            strcmp(val->as.member_access.object->as.variable.name, "Type") == 0) {
            XrTypeKind k = type_member_to_kind(val->as.member_access.name);
            if (k != XR_KIND_COUNT && *count < cap) {
                kinds[(*count)++] = k;
                if (k == XR_KIND_STRUCT_OBJECT && *count < cap)
                    kinds[(*count)++] = XR_KIND_JSON;
            }
        }
    } else if (pattern->type == AST_MEMBER_ACCESS && pattern->as.member_access.object &&
               pattern->as.member_access.object->type == AST_VARIABLE &&
               pattern->as.member_access.object->as.variable.name &&
               strcmp(pattern->as.member_access.object->as.variable.name, "Type") == 0) {
        XrTypeKind k = type_member_to_kind(pattern->as.member_access.name);
        if (k != XR_KIND_COUNT && *count < cap) {
            kinds[(*count)++] = k;
            if (k == XR_KIND_STRUCT_OBJECT && *count < cap)
                kinds[(*count)++] = XR_KIND_JSON;
        }
    } else if (pattern->type == AST_PATTERN_MULTI) {
        PatternMultiNode *multi = &pattern->as.pattern_multi;
        for (int i = 0; i < multi->count; i++) {
            collect_matched_type_members(multi->patterns[i], kinds, count, cap);
        }
    }
}

// Helper: add enum member name to collection
/* Name of a type that may denote an enum by name: a monomorphized generic
 * enum arrives as an instance type, and the parser resolves a bare enum name
 * to a class type. Both carry the declared name. */
static const char *pattern_named_type_name(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS)
        return type->instance.class_name;
    return NULL;
}

static XrType *pattern_enum_type_by_name(XaInferContext *ctx, const char *name) {
    if (!ctx || !ctx->analyzer || !name)
        return NULL;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
    if (!sym || sym->kind != XA_SYM_ENUM)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links && links->type && XR_TYPE_IS_ENUM(links->type))
        return links->type;
    return NULL;
}

/* One `Enum.Variant` written in a match arm. The site is kept alongside the
 * name so a diagnostic can point at the arm that wrote it, not at the match. */
typedef struct XaMatchedEnumMember {
    const char *name;
    AstNode *site;
} XaMatchedEnumMember;

static void add_enum_member(const char *member, AstNode *site, XaMatchedEnumMember **members,
                            int *count, int *cap) {
    if (!member)
        return;
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *members = xr_realloc(*members, sizeof(XaMatchedEnumMember) * (*cap));
    }
    (*members)[*count].name = member;
    (*members)[*count].site = site;
    (*count)++;
}

// Helper: collect enum member names from a match arm pattern
static void collect_matched_enum_members(AstNode *pattern, XaMatchedEnumMember **members,
                                         int *count, int *cap) {
    if (!pattern)
        return;

    if (pattern->type == AST_ENUM_ACCESS) {
        add_enum_member(pattern->as.enum_access.member_name, pattern, members, count, cap);
    } else if (pattern->type == AST_MEMBER_ACCESS) {
        add_enum_member(pattern->as.member_access.name, pattern, members, count, cap);
    } else if (pattern->type == AST_PATTERN_LITERAL && pattern->as.pattern_literal.value) {
        // Unwrap: AST_PATTERN_LITERAL wrapping AST_MEMBER_ACCESS or AST_ENUM_ACCESS
        collect_matched_enum_members(pattern->as.pattern_literal.value, members, count, cap);
    } else if (pattern->type == AST_PATTERN_ADT) {
        /* ADT variant pattern: extract member name from variant node */
        AstNode *variant = pattern->as.pattern_adt.variant;
        if (variant) {
            if (variant->type == AST_ENUM_ACCESS)
                add_enum_member(variant->as.enum_access.member_name, variant, members, count, cap);
            else if (variant->type == AST_MEMBER_ACCESS)
                add_enum_member(variant->as.member_access.name, variant, members, count, cap);
        }
    } else if (pattern->type == AST_PATTERN_MULTI) {
        PatternMultiNode *multi = &pattern->as.pattern_multi;
        for (int i = 0; i < multi->count; i++) {
            collect_matched_enum_members(multi->patterns[i], members, count, cap);
        }
    }
}

// Detect whether the pattern introduces any name bindings.
// Top-level `name => ...` and any AST_VARIABLE buried inside a tuple
// pattern both count; everything else (literals, ranges, wildcards,
// alternations) is binding-free.
XR_FUNC bool xa_pattern_has_binding(AstNode *pattern) {
    if (!pattern)
        return false;
    if (pattern->type == AST_PATTERN_LITERAL) {
        AstNode *pval = pattern->as.pattern_literal.value;
        return pval && pval->type == AST_VARIABLE;
    }
    if (pattern->type == AST_PATTERN_TUPLE) {
        PatternTupleNode *tp = &pattern->as.pattern_tuple;
        for (int i = 0; i < tp->count; i++) {
            if (xa_pattern_has_binding(tp->patterns[i]))
                return true;
        }
    }
    if (pattern->type == AST_PATTERN_ADT) {
        PatternAdtNode *ap = &pattern->as.pattern_adt;
        for (int i = 0; i < ap->count; i++) {
            if (xa_pattern_has_binding(ap->patterns[i]))
                return true;
        }
    }
    if (pattern->type == AST_PATTERN_OBJECT) {
        PatternObjectNode *op = &pattern->as.pattern_object;
        for (int i = 0; i < op->count; i++) {
            if (xa_pattern_has_binding(op->patterns[i]))
                return true;
        }
    }
    if (pattern->type == AST_PATTERN_ARRAY) {
        PatternArrayNode *ap = &pattern->as.pattern_array;
        if (ap->rest_name)
            return true;
        for (int i = 0; i < ap->count; i++) {
            if (xa_pattern_has_binding(ap->patterns[i]))
                return true;
        }
    }
    if (pattern->type == AST_PATTERN_TYPE) {
        return pattern->as.pattern_type.binding_name != NULL;
    }
    return false;
}

// An array match pattern is matched only by its length (element reads trap out
// of bounds, so refutable per-element tests are not run). Element sub-patterns
// must therefore be irrefutable (bindings, wildcards, or nested irrefutable
// destructure). Returns true for an irrefutable sub-pattern.
static bool pattern_is_irrefutable(AstNode *pattern) {
    if (!pattern)
        return true;
    switch (pattern->type) {
        case AST_PATTERN_WILDCARD:
            return true;
        case AST_PATTERN_LITERAL: {
            AstNode *pval = pattern->as.pattern_literal.value;
            return pval && pval->type == AST_VARIABLE;  // bare-name binding
        }
        case AST_PATTERN_TUPLE: {
            PatternTupleNode *tp = &pattern->as.pattern_tuple;
            for (int i = 0; i < tp->count; i++) {
                if (!pattern_is_irrefutable(tp->patterns[i]))
                    return false;
            }
            return true;
        }
        case AST_PATTERN_OBJECT: {
            PatternObjectNode *op = &pattern->as.pattern_object;
            for (int i = 0; i < op->count; i++) {
                if (!pattern_is_irrefutable(op->patterns[i]))
                    return false;
            }
            return true;
        }
        case AST_PATTERN_ARRAY: {
            PatternArrayNode *ap = &pattern->as.pattern_array;
            for (int i = 0; i < ap->count; i++) {
                if (!pattern_is_irrefutable(ap->patterns[i]))
                    return false;
            }
            return true;
        }
        default:
            return false;  // literals, ranges, ADT, type tests are refutable
    }
}

// Reject refutable element sub-patterns inside array patterns (recursively).
static void xa_check_array_pattern_elements(XaInferContext *ctx, AstNode *pattern) {
    if (!ctx || !pattern)
        return;
    switch (pattern->type) {
        case AST_PATTERN_TUPLE: {
            PatternTupleNode *tp = &pattern->as.pattern_tuple;
            for (int i = 0; i < tp->count; i++)
                xa_check_array_pattern_elements(ctx, tp->patterns[i]);
            break;
        }
        case AST_PATTERN_OBJECT: {
            PatternObjectNode *op = &pattern->as.pattern_object;
            for (int i = 0; i < op->count; i++)
                xa_check_array_pattern_elements(ctx, op->patterns[i]);
            break;
        }
        case AST_PATTERN_ARRAY: {
            PatternArrayNode *ap = &pattern->as.pattern_array;
            for (int i = 0; i < ap->count; i++) {
                AstNode *sub = ap->patterns[i];
                if (sub && !pattern_is_irrefutable(sub)) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = sub->line, .column = sub->column};
                    xa_analyzer_add_diagnostic(
                        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                        "array pattern elements must be bindings or wildcards; use an `if` guard "
                        "to test element values",
                        &loc);
                }
                xa_check_array_pattern_elements(ctx, sub);
            }
            break;
        }
        default:
            break;
    }
}

// Range patterns cover integer subjects only: floating-point endpoints have
// no exact-step semantics and NaN breaks the ordering the lowering relies on,
// so they are rejected instead of silently accepted.
static void xa_check_range_pattern_endpoint(XaInferContext *ctx, AstNode *endpoint) {
    if (!ctx || !endpoint)
        return;
    XrType *t = xa_visit_infer_expr(ctx, endpoint);
    if (t && XR_TYPE_IS_FLOAT(t)) {
        XrLocation loc = {
            .file = ctx->file_path, .line = endpoint->line, .column = endpoint->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   "range pattern endpoints must be integers; use an `if` guard "
                                   "for floating-point comparisons",
                                   &loc);
    }
}

static void xa_check_range_pattern_operands(XaInferContext *ctx, AstNode *pattern) {
    if (!ctx || !pattern)
        return;
    switch (pattern->type) {
        case AST_PATTERN_RANGE:
            xa_check_range_pattern_endpoint(ctx, pattern->as.pattern_range.start);
            xa_check_range_pattern_endpoint(ctx, pattern->as.pattern_range.end);
            break;
        case AST_PATTERN_MULTI: {
            PatternMultiNode *mp = &pattern->as.pattern_multi;
            for (int i = 0; i < mp->count; i++)
                xa_check_range_pattern_operands(ctx, mp->patterns[i]);
            break;
        }
        case AST_PATTERN_TUPLE: {
            PatternTupleNode *tp = &pattern->as.pattern_tuple;
            for (int i = 0; i < tp->count; i++)
                xa_check_range_pattern_operands(ctx, tp->patterns[i]);
            break;
        }
        default:
            break;
    }
}

static void report_enum_pattern_error(XaInferContext *ctx, AstNode *node, int code,
                                      const char *message) {
    XrLocation location = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, message, &location);
}

static const char *enum_pattern_variant_name(AstNode *variant) {
    if (!variant)
        return NULL;
    if (variant->type == AST_ENUM_ACCESS)
        return variant->as.enum_access.member_name;
    if (variant->type == AST_MEMBER_ACCESS)
        return variant->as.member_access.name;
    return NULL;
}

static const char *enum_pattern_owner_name(AstNode *variant) {
    if (!variant)
        return NULL;
    if (variant->type == AST_ENUM_ACCESS)
        return variant->as.enum_access.enum_name;
    if (variant->type == AST_MEMBER_ACCESS && variant->as.member_access.object &&
        variant->as.member_access.object->type == AST_VARIABLE)
        return variant->as.member_access.object->as.variable.name;
    return NULL;
}

static XaSymbol *enum_pattern_symbol(XaInferContext *ctx, AstNode *variant, XrType *slot_type,
                                     uint16_t *ordinal_out) {
    const XaSelection *selection = xa_analyzer_get_selection(ctx->analyzer, variant);
    if (selection && selection->kind == XA_SEL_ENUM_MEMBER && selection->target_symbol &&
        selection->target_symbol->kind == XA_SYM_ENUM && selection->field_index >= 0 &&
        selection->field_index <= UINT16_MAX) {
        *ordinal_out = (uint16_t) selection->field_index;
        return selection->target_symbol;
    }

    const char *enum_name =
        slot_type && XR_TYPE_IS_ENUM(slot_type) ? slot_type->enum_type.enum_name : NULL;
    if (!enum_name)
        enum_name = enum_pattern_owner_name(variant);
    const char *variant_name = enum_pattern_variant_name(variant);
    if (!enum_name || !variant_name)
        return NULL;
    XaSymbol *symbol = xa_lookup_visible_symbol(ctx, enum_name);
    if (!symbol || symbol->kind != XA_SYM_ENUM)
        symbol = xa_analyzer_lookup_deep(ctx->analyzer, enum_name);
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    int ordinal = xa_enum_info_find_variant(links ? links->enum_info : NULL, variant_name);
    if (!symbol || symbol->kind != XA_SYM_ENUM || ordinal < 0 || ordinal > UINT16_MAX)
        return NULL;
    *ordinal_out = (uint16_t) ordinal;
    return symbol;
}

static int enum_pattern_payload_slot(const XaEnumVariantInfo *variant, const char *field_name) {
    if (!variant || !variant->payload_names || !field_name)
        return -1;
    for (uint16_t i = 0; i < variant->payload_count; i++) {
        if (variant->payload_names[i] && strcmp(variant->payload_names[i], field_name) == 0)
            return (int) i;
    }
    return -1;
}

XR_FUNC bool xa_resolve_enum_pattern_plans(XaInferContext *ctx, AstNode *pattern,
                                           XrType *slot_type) {
    if (!ctx || !ctx->analyzer || !pattern)
        return true;

    if (pattern->type == AST_PATTERN_LITERAL && pattern->as.pattern_literal.value) {
        AstNode *variant_path = pattern->as.pattern_literal.value;
        if (variant_path->type != AST_ENUM_ACCESS && variant_path->type != AST_MEMBER_ACCESS)
            return true;
        uint16_t ordinal = 0;
        XaSymbol *symbol = enum_pattern_symbol(ctx, variant_path, slot_type, &ordinal);
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
        XaEnumInfo *info = links ? links->enum_info : NULL;
        if (!symbol || !info || !info->is_payload_enum || !info->variants ||
            ordinal >= info->variant_count)
            return true;
        XaEnumVariantInfo *variant = &info->variants[ordinal];
        if (variant->payload_count != 0) {
            char message[224];
            snprintf(message, sizeof(message),
                     "payload enum variant '%s.%s' requires a named-field pattern",
                     symbol->name ? symbol->name : "?", variant->name ? variant->name : "?");
            report_enum_pattern_error(ctx, pattern, XR_ERR_ANALYZE_TYPE_MISMATCH, message);
            return false;
        }
        XaEnumRecordPlan plan = {
            .kind = XA_ENUM_VARIANT_PATTERN,
            .enum_symbol_id = symbol->id,
            .enum_layout_id = info->layout ? info->layout->layout_id : 0,
            .variant_ordinal = ordinal,
            .source_field_count = 0,
            .declaration_field_count = 0,
            .source_to_slot = NULL,
            .complete = true,
        };
        if (!xa_enum_record_plan_table_set(
                (XaEnumRecordPlanTable *) ctx->analyzer->enum_record_plan_table, pattern, &plan)) {
            report_enum_pattern_error(ctx, pattern, XR_ERR_ANALYZE,
                                      "failed to publish immutable enum unit-pattern plan");
            return false;
        }
        return true;
    }

    if (pattern->type == AST_PATTERN_ADT) {
        PatternAdtNode *adt = &pattern->as.pattern_adt;
        uint16_t ordinal = 0;
        XaSymbol *symbol = enum_pattern_symbol(ctx, adt->variant, slot_type, &ordinal);
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
        XaEnumInfo *info = links ? links->enum_info : NULL;
        if (!symbol || !info || !info->variants || ordinal >= info->variant_count) {
            report_enum_pattern_error(ctx, pattern, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                      "enum record pattern requires an exact enum variant path");
            return false;
        }
        XaEnumVariantInfo *variant = &info->variants[ordinal];
        if (variant->payload_count == 0) {
            char message[224];
            snprintf(message, sizeof(message),
                     "unit enum variant '%s.%s' has no payload and cannot use '{}' in a pattern",
                     symbol->name ? symbol->name : "?", variant->name ? variant->name : "?");
            report_enum_pattern_error(ctx, pattern, XR_ERR_ANALYZE_TYPE_MISMATCH, message);
            return false;
        }

        uint16_t source_count = adt->count > UINT16_MAX ? UINT16_MAX : (uint16_t) adt->count;
        uint16_t *source_to_slot =
            source_count ? xr_malloc((size_t) source_count * sizeof(*source_to_slot)) : NULL;
        bool *seen = xr_calloc((size_t) variant->payload_count, sizeof(*seen));
        bool complete =
            source_count == (uint16_t) adt->count && seen && (source_count == 0 || source_to_slot);
        for (uint16_t i = 0; i < source_count; i++)
            source_to_slot[i] = UINT16_MAX;
        for (int i = 0; i < adt->count; i++) {
            const char *field_name = adt->field_names ? adt->field_names[i] : NULL;
            int slot = enum_pattern_payload_slot(variant, field_name);
            if (slot < 0) {
                char message[256];
                snprintf(message, sizeof(message), "enum variant '%s.%s' has no field '%s'",
                         symbol->name ? symbol->name : "?", variant->name ? variant->name : "?",
                         field_name ? field_name : "<missing>");
                report_enum_pattern_error(ctx, pattern, XR_ERR_ANALYZE_UNDEFINED_VAR, message);
                complete = false;
                continue;
            }
            if (seen && seen[slot]) {
                char message[256];
                snprintf(message, sizeof(message),
                         "enum pattern field '%s.%s.%s' is listed more than once",
                         symbol->name ? symbol->name : "?", variant->name ? variant->name : "?",
                         field_name);
                report_enum_pattern_error(ctx, pattern, XR_ERR_ANALYZE_ARG_TYPE, message);
                complete = false;
            } else if (seen) {
                seen[slot] = true;
            }
            if (source_to_slot && i < source_count)
                source_to_slot[i] = (uint16_t) slot;
        }

        if (complete) {
            XaEnumRecordPlan plan = {
                .kind = XA_ENUM_VARIANT_PATTERN,
                .enum_symbol_id = symbol->id,
                .enum_layout_id = info->layout ? info->layout->layout_id : 0,
                .variant_ordinal = ordinal,
                .source_field_count = source_count,
                .declaration_field_count = variant->payload_count,
                .source_to_slot = source_to_slot,
                .complete = true,
            };
            complete = xa_enum_record_plan_table_set(
                (XaEnumRecordPlanTable *) ctx->analyzer->enum_record_plan_table, pattern, &plan);
            if (!complete)
                report_enum_pattern_error(ctx, pattern, XR_ERR_ANALYZE,
                                          "failed to publish immutable enum pattern plan");
        }

        bool children_complete = true;
        for (int i = 0; i < adt->count; i++) {
            int slot =
                source_to_slot && i < source_count && source_to_slot[i] < variant->payload_count
                    ? source_to_slot[i]
                    : -1;
            XrType *payload_type = slot >= 0 ? xa_analyzer_resolve_adt_payload_type(
                                                   ctx->analyzer, slot_type, adt->variant, slot)
                                             : NULL;
            children_complete =
                xa_resolve_enum_pattern_plans(ctx, adt->patterns[i], payload_type) &&
                children_complete;
        }
        xr_free(seen);
        xr_free(source_to_slot);
        return complete && children_complete;
    }

    AstNode **children = NULL;
    int count = 0;
    if (pattern->type == AST_PATTERN_TUPLE) {
        children = pattern->as.pattern_tuple.patterns;
        count = pattern->as.pattern_tuple.count;
    } else if (pattern->type == AST_PATTERN_OBJECT) {
        children = pattern->as.pattern_object.patterns;
        count = pattern->as.pattern_object.count;
    } else if (pattern->type == AST_PATTERN_ARRAY) {
        children = pattern->as.pattern_array.patterns;
        count = pattern->as.pattern_array.count;
    } else if (pattern->type == AST_PATTERN_MULTI) {
        children = pattern->as.pattern_multi.patterns;
        count = pattern->as.pattern_multi.count;
    }
    bool complete = true;
    for (int i = 0; i < count; i++)
        complete = xa_resolve_enum_pattern_plans(ctx, children[i], NULL) && complete;
    return complete;
}

// Recursively register binding symbols introduced by `pattern` into the
// current scope. `slot_type` is the static type of the value flowing
// into this position; for tuple sub-slots it's drawn from the tuple's
// declared element types when the scrutinee is a known tuple type.
XR_FUNC void xa_register_pattern_bindings(XaInferContext *ctx, AstNode *pattern,
                                          XrType *slot_type) {
    if (!ctx || !pattern)
        return;

    if (pattern->type == AST_PATTERN_LITERAL) {
        AstNode *pval = pattern->as.pattern_literal.value;
        if (pval && pval->type == AST_VARIABLE) {
            XaSymbol *bind_sym = xa_symbol_new(pval->as.variable.name, XA_SYM_VARIABLE);
            bind_sym->location.line = pval->line;
            xa_visit_add_symbol_checked(ctx, bind_sym, 0);
            XaSymbolLinks *bind_links = xa_analyzer_get_links(ctx->analyzer, bind_sym);
            if (bind_links) {
                bind_links->type = slot_type ? slot_type : xr_type_new_unknown(NULL);
                bind_links->is_definitely_assigned = true;
            }
            pval->as.variable.symbol_id = bind_sym->id;
        }
        return;
    }

    if (pattern->type == AST_PATTERN_TUPLE) {
        PatternTupleNode *tp = &pattern->as.pattern_tuple;
        for (int i = 0; i < tp->count; i++) {
            AstNode *sub = tp->patterns[i];
            if (!sub)
                continue;
            XrType *elem_type = NULL;
            if (slot_type && XR_TYPE_IS_TUPLE(slot_type))
                elem_type = xr_type_tuple_get(slot_type, i);
            xa_register_pattern_bindings(ctx, sub, elem_type);
        }
    }

    /* ADT variant destructure: bind each payload slot with the enum
     * member's declared type, substituting generic enum arguments from
     * the current match subject. */
    if (pattern->type == AST_PATTERN_ADT) {
        PatternAdtNode *ap = &pattern->as.pattern_adt;
        const XaEnumRecordPlan *plan = xa_analyzer_get_enum_record_plan(ctx->analyzer, pattern);
        for (int i = 0; i < ap->count; i++) {
            AstNode *sub = ap->patterns[i];
            if (!sub)
                continue;
            int slot = plan && plan->complete && i < plan->source_field_count &&
                               plan->source_to_slot[i] < plan->declaration_field_count
                           ? plan->source_to_slot[i]
                           : -1;
            XrType *payload_type = slot >= 0 ? xa_analyzer_resolve_adt_payload_type(
                                                   ctx->analyzer, slot_type, ap->variant, slot)
                                             : NULL;
            xa_register_pattern_bindings(ctx, sub, payload_type);
        }
    }

    /* Object pattern: bind each field's sub-pattern. Field slot type is drawn
     * from the subject's static fields when known (Json carries nullable field
     * types); otherwise unknown. */
    if (pattern->type == AST_PATTERN_OBJECT) {
        PatternObjectNode *op = &pattern->as.pattern_object;
        for (int i = 0; i < op->count; i++) {
            AstNode *sub = op->patterns[i];
            if (!sub)
                continue;
            XrType *field_type = NULL;
            if (slot_type && XR_TYPE_HAS_OBJECT_SHAPE(slot_type) &&
                slot_type->object.field_count > 0 && slot_type->object.field_names &&
                slot_type->object.field_types) {
                for (int f = 0; f < slot_type->object.field_count; f++) {
                    if (slot_type->object.field_names[f] && op->field_names[i] &&
                        strcmp(slot_type->object.field_names[f], op->field_names[i]) == 0) {
                        XrType *ft = slot_type->object.field_types[f];
                        field_type = ft && XR_TYPE_IS_JSON(slot_type)
                                         ? xr_type_make_nullable(ctx->analyzer->isolate, ft)
                                         : ft;
                        break;
                    }
                }
            }
            xa_register_pattern_bindings(ctx, sub, field_type);
        }
    }

    /* Array pattern: bind positional element sub-patterns with the array's
     * element type, and the optional rest binding as a borrowed Slice<element>. */
    if (pattern->type == AST_PATTERN_ARRAY) {
        PatternArrayNode *ap = &pattern->as.pattern_array;
        XrType *elem_type = NULL;
        if (slot_type && XR_TYPE_IS_ARRAY(slot_type))
            elem_type = slot_type->container.element_type;
        for (int i = 0; i < ap->count; i++) {
            if (ap->patterns[i])
                xa_register_pattern_bindings(ctx, ap->patterns[i], elem_type);
        }
        if (ap->rest_name) {
            XaSymbol *rest_sym = xa_symbol_new(ap->rest_name, XA_SYM_VARIABLE);
            rest_sym->location.line = pattern->line;
            xa_visit_add_symbol_checked(ctx, rest_sym, 0);
            ap->rest_symbol_id = rest_sym->id;
            XaSymbolLinks *rest_links = xa_analyzer_get_links(ctx->analyzer, rest_sym);
            if (rest_links) {
                rest_links->type = xr_type_new_slice(
                    ctx->analyzer->isolate, elem_type ? elem_type : xr_type_new_unknown(NULL));
                rest_links->is_definitely_assigned = true;
            }
        }
    }

    /* Type pattern: `is T name` introduces a narrowed binding of type T. */
    if (pattern->type == AST_PATTERN_TYPE) {
        PatternTypeNode *tp = &pattern->as.pattern_type;
        if (tp->binding_name) {
            XaSymbol *bind_sym = xa_symbol_new(tp->binding_name, XA_SYM_VARIABLE);
            bind_sym->location.line = pattern->line;
            xa_visit_add_symbol_checked(ctx, bind_sym, 0);
            XaSymbolLinks *bind_links = xa_analyzer_get_links(ctx->analyzer, bind_sym);
            if (bind_links) {
                bind_links->type = xr_tref_resolve_in_analyzer(ctx->analyzer, tp->type);
                bind_links->is_definitely_assigned = true;
            }
            tp->symbol_id = bind_sym->id;
        }
    }
}

XrType *xa_visit_match_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    MatchExprNode *match = &node->as.match_expr;

    // Infer subject type
    XrType *subject_type = NULL;
    if (match->expr) {
        subject_type = xa_visit_infer_expr(ctx, match->expr);
    }

    // Collect arm body types and union them
    if (match->arm_count == 0) {
        return xr_type_new_never(NULL);
    }

    XrType *result = NULL;
    bool has_wildcard = false;

    for (int i = 0; i < match->arm_count; i++) {
        AstNode *arm = match->arms[i];
        if (!arm || arm->type != AST_MATCH_ARM)
            continue;

        MatchArmNode *arm_node = &arm->as.match_arm;

        // Check for wildcard pattern
        if (arm_node->pattern && arm_node->pattern->type == AST_PATTERN_WILDCARD) {
            has_wildcard = true;
        }

        // Register binding variables in the pattern. Both top-level
        // bare-name patterns (`n if (n < 0) => ...`) and identifiers
        // captured by a tuple destructure (`(x, _) => x + 1`) need
        // fresh scoped symbols typed from the matching subject slot.
        xa_check_array_pattern_elements(ctx, arm_node->pattern);
        xa_check_range_pattern_operands(ctx, arm_node->pattern);
        xa_resolve_enum_pattern_plans(ctx, arm_node->pattern, subject_type);

        bool has_binding = xa_pattern_has_binding(arm_node->pattern);
        if (has_binding) {
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, arm);
            xa_register_pattern_bindings(ctx, arm_node->pattern, subject_type);
        }

        // Infer guard if present
        if (arm_node->guard) {
            XrType *guard_type = xa_visit_infer_expr(ctx, arm_node->guard);
            xa_check_condition_type(ctx, arm_node->guard, guard_type);
        }

        // Infer body type
        XrType *arm_type = xr_type_new_unknown(NULL);
        if (arm_node->body) {
            if (arm_node->body->type == AST_BLOCK) {
                xa_visit_block_stmt(ctx, arm_node->body);
            } else {
                arm_type = xa_visit_infer_expr(ctx, arm_node->body);
            }
        }

        if (has_binding) {
            xa_analyzer_exit_scope(ctx->analyzer);
        }

        if (!result) {
            result = arm_type;
        } else if (!xr_type_equals(result, arm_type)) {
            result = xr_type_union(ctx->analyzer->isolate, result, arm_type);
        }
    }

    // Exhaustiveness check for enum types
    // Resolve subject to enum type if possible:
    // 1. subject_type is already XR_KIND_ENUM
    // 2. subject is a variable whose declared_type resolves to an enum
    //    (parser creates XR_KIND_CLASS for "Color", need to check if it's actually an enum)
    if (subject_type && !XR_TYPE_IS_ENUM(subject_type)) {
        /* A generic enum reaches here monomorphized as an instance type, and
         * the parser resolves a bare enum name to a class type. Both name the
         * enum, so the symbol table decides. */
        XrType *named = pattern_enum_type_by_name(ctx, pattern_named_type_name(subject_type));
        if (named)
            subject_type = named;
    }
    if (!has_wildcard && subject_type && !XR_TYPE_IS_ENUM(subject_type) && match->expr &&
        match->expr->type == AST_VARIABLE) {
        const char *var_name = match->expr->as.variable.name;
        XaSymbol *var_sym = xa_scope_lookup(ctx->analyzer->current_scope, var_name);
        if (var_sym) {
            XaSymbolLinks *var_links = xa_analyzer_get_links(ctx->analyzer, var_sym);
            XrType *dt = var_links ? var_links->declared_type : NULL;
            if (dt) {
                if (XR_TYPE_IS_ENUM(dt)) {
                    subject_type = dt;
                } else {
                    XrType *named_dt = pattern_enum_type_by_name(ctx, pattern_named_type_name(dt));
                    if (named_dt)
                        subject_type = named_dt;
                }
            }
        }
    }
    if (subject_type && XR_TYPE_IS_ENUM(subject_type)) {
        const char *enum_name = subject_type->enum_type.enum_name;
        if (enum_name) {
            XaSymbol *enum_sym = xa_scope_lookup(ctx->analyzer->current_scope, enum_name);
            if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
                XaSymbolLinks *enum_links = xa_analyzer_get_links(ctx->analyzer, enum_sym);

                XaEnumInfo *enum_info = enum_links ? enum_links->enum_info : NULL;
                if (enum_info && enum_info->variant_count > 0) {
                    // Collect matched members from all arms
                    XaMatchedEnumMember *matched = NULL;
                    int matched_count = 0, matched_cap = 0;

                    for (int i = 0; i < match->arm_count; i++) {
                        AstNode *arm = match->arms[i];
                        if (!arm || arm->type != AST_MATCH_ARM)
                            continue;
                        collect_matched_enum_members(arm->as.match_arm.pattern, &matched,
                                                     &matched_count, &matched_cap);
                    }

                    /* A name the enum does not declare is a typo, not an arm
                     * that never fires. Expression position already rejects
                     * `Color.Purple`; a pattern must answer the same way, and
                     * independently of `_`, which otherwise hides it. */
                    for (int j = 0; j < matched_count; j++) {
                        bool declared = false;
                        for (uint32_t i = 0; i < enum_info->variant_count; i++) {
                            const char *variant = enum_info->variants[i].name;
                            if (variant && strcmp(matched[j].name, variant) == 0) {
                                declared = true;
                                break;
                            }
                        }
                        if (declared)
                            continue;
                        AstNode *site = matched[j].site ? matched[j].site : node;
                        XrLocation loc = {
                            .file = ctx->file_path, .line = site->line, .column = site->column};
                        char msg[256];
                        snprintf(msg, sizeof(msg), "enum '%s' has no member '%s'", enum_name,
                                 matched[j].name);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
                    }

                    // Check which enum members are missing
                    for (uint32_t i = 0; !has_wildcard && i < enum_info->variant_count; i++) {
                        const char *member_name = enum_info->variants[i].name;
                        if (!member_name)
                            continue;

                        bool found = false;
                        for (int j = 0; j < matched_count; j++) {
                            if (strcmp(matched[j].name, member_name) == 0) {
                                found = true;
                                break;
                            }
                        }

                        if (!found) {
                            XrLocation loc = {
                                .file = ctx->file_path, .line = node->line, .column = node->column};
                            char msg[256];
                            snprintf(msg, sizeof(msg), "Non-exhaustive match: missing '%s.%s'",
                                     enum_name, member_name);
                            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                       XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE, msg,
                                                       &loc);
                        }
                    }

                    if (matched)
                        xr_free(matched);
                }
            }
        }
    }

    // Exhaustiveness check for typeof match on union/nullable types
    // Detects: match typeof(x) { Type.i64 => ..., Type.string => ... }
    if (!has_wildcard && match->expr) {
        const char *typeof_var = get_typeof_arg_name(match->expr);
        if (typeof_var) {
            // Look up the static type of the variable
            XaSymbol *var_sym = xa_scope_lookup(ctx->analyzer->current_scope, typeof_var);
            XrType *var_type = NULL;
            if (var_sym) {
                XaSymbolLinks *var_links = xa_analyzer_get_links(ctx->analyzer, var_sym);
                if (var_links)
                    var_type =
                        var_links->declared_type ? var_links->declared_type : var_links->type;
            }

            if (var_type) {
                // Build expected type kind set from the variable's static type
                XrTypeKind expected[XR_UNION_MAX_MEMBERS + 1];
                int expected_count = 0;

                if (XR_TYPE_IS_UNION(var_type)) {
                    for (int i = 0; i < var_type->union_type.member_count &&
                                    expected_count < XR_UNION_MAX_MEMBERS;
                         i++) {
                        XrType *m = var_type->union_type.members[i];
                        if (m)
                            expected[expected_count++] = m->kind;
                    }
                } else if (var_type->is_nullable) {
                    // T? means we need T and null
                    XrType *base = xr_type_non_nullable(ctx->analyzer->isolate, var_type);
                    if (base)
                        expected[expected_count++] = base->kind;
                    expected[expected_count++] = XR_KIND_NULL;
                }

                if (expected_count > 0) {
                    // Collect matched Type.xxx kinds from arms
                    XrTypeKind matched_kinds[32];
                    int matched_count = 0;

                    for (int i = 0; i < match->arm_count; i++) {
                        AstNode *arm = match->arms[i];
                        if (!arm || arm->type != AST_MATCH_ARM)
                            continue;
                        collect_matched_type_members(arm->as.match_arm.pattern, matched_kinds,
                                                     &matched_count, 32);
                    }

                    // Check which expected types are missing
                    for (int i = 0; i < expected_count; i++) {
                        bool found = false;
                        for (int j = 0; j < matched_count; j++) {
                            if (matched_kinds[j] == expected[i]) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            const char *missing = kind_to_type_member(expected[i]);
                            if (missing) {
                                XrLocation loc = {.file = ctx->file_path,
                                                  .line = node->line,
                                                  .column = node->column};
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                         "Non-exhaustive match: missing '%s' for type '%s'",
                                         missing, xr_type_to_string(var_type));
                                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                           XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE, msg,
                                                           &loc);
                            }
                        }
                    }
                }
            }
        }
    }

    return result ? result : xr_type_new_unknown(NULL);
}
