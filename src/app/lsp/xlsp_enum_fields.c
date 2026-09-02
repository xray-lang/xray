/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xlsp_enum_fields.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_ast_visitor.h"
#include "../../frontend/analyzer/xanalyzer_symbol.h"
#include "../../frontend/parser/xast_nodes.h"
#include <string.h>

bool xlsp_enum_field_identity_equal(XlspEnumFieldIdentity left,
                                    XlspEnumFieldIdentity right) {
    return left.enum_symbol_id == right.enum_symbol_id &&
           left.variant_ordinal == right.variant_ordinal && left.field_slot == right.field_slot;
}

static XaEnumVariantInfo *variant_for_identity(XaAnalyzer *analyzer,
                                               XlspEnumFieldIdentity identity,
                                               XaSymbol **out_symbol) {
    if (!analyzer || !analyzer->global_scope || identity.enum_symbol_id == 0)
        return NULL;
    XaSymbol *symbol = xa_scope_lookup_by_id(analyzer->global_scope, identity.enum_symbol_id);
    XaEnumInfo *info = symbol ? symbol->links.enum_info : NULL;
    if (!info || identity.variant_ordinal >= info->variant_count)
        return NULL;
    XaEnumVariantInfo *variant = &info->variants[identity.variant_ordinal];
    if (identity.field_slot >= variant->payload_count || !variant->payload_names ||
        !variant->payload_names[identity.field_slot])
        return NULL;
    if (out_symbol)
        *out_symbol = symbol;
    return variant;
}

static bool occurrence_from_identity(XaAnalyzer *analyzer, XlspEnumFieldIdentity identity,
                                     XrLocation location, bool is_declaration,
                                     XlspEnumFieldOccurrence *out) {
    XaSymbol *symbol = NULL;
    XaEnumVariantInfo *variant = variant_for_identity(analyzer, identity, &symbol);
    if (!variant || !out || !location.file || location.line == 0 || location.column == 0)
        return false;
    const char *name = variant->payload_names[identity.field_slot];
    if (location.end_line == 0)
        location.end_line = location.line;
    if (location.end_column == 0)
        location.end_column = location.column + (uint32_t) strlen(name);
    *out = (XlspEnumFieldOccurrence){
        .identity = identity,
        .enum_name = symbol->name,
        .variant_name = variant->name,
        .field_name = name,
        .field_type = variant->payload_types ? variant->payload_types[identity.field_slot] : NULL,
        .location = location,
        .is_declaration = is_declaration,
    };
    return true;
}

bool xlsp_enum_field_definition(XaAnalyzer *analyzer, XlspEnumFieldIdentity identity,
                                XlspEnumFieldOccurrence *out) {
    XaEnumVariantInfo *variant = variant_for_identity(analyzer, identity, NULL);
    if (!variant || !variant->payload_locations)
        return false;
    return occurrence_from_identity(analyzer, identity,
                                    variant->payload_locations[identity.field_slot], true, out);
}

static bool position_in_name(XrLspPosition position, XrNameSpan span, const char *name) {
    if (!name || span.line <= 0 || span.column <= 0 || position.line + 1 != (uint32_t) span.line)
        return false;
    uint32_t column = position.character + 1;
    uint32_t start = (uint32_t) span.column;
    return column >= start && column < start + (uint32_t) strlen(name);
}

static bool plan_occurrence(XaAnalyzer *analyzer, AstNode *node, const char *document_uri,
                            int source_index, XrNameSpan span,
                            XlspEnumFieldOccurrence *out) {
    const XaEnumRecordPlan *plan = xa_analyzer_get_enum_record_plan(analyzer, node);
    if (!plan || !plan->complete || source_index < 0 ||
        source_index >= plan->source_field_count || !plan->source_to_slot)
        return false;
    XlspEnumFieldIdentity identity = {
        .enum_symbol_id = plan->enum_symbol_id,
        .variant_ordinal = plan->variant_ordinal,
        .field_slot = plan->source_to_slot[source_index],
    };
    XrLocation location = {
        .file = document_uri,
        .line = (uint32_t) span.line,
        .column = (uint32_t) span.column,
    };
    return occurrence_from_identity(analyzer, identity, location, false, out);
}

typedef struct FieldAtContext {
    XaAnalyzer *analyzer;
    const char *document_uri;
    XrLspPosition position;
    XlspEnumFieldOccurrence *out;
    bool found;
} FieldAtContext;

static void find_field_at_node(AstNode *node, void *raw_ctx) {
    FieldAtContext *ctx = raw_ctx;
    if (!node || !ctx || ctx->found)
        return;

    if (node->type == AST_ENUM_DECL) {
        EnumDeclNode *declaration = &node->as.enum_decl;
        for (int ordinal = 0; ordinal < declaration->member_count; ordinal++) {
            AstNode *member = declaration->members[ordinal];
            if (!member || member->type != AST_ENUM_MEMBER)
                continue;
            EnumMemberNode *variant = &member->as.enum_member;
            for (int slot = 0; slot < variant->payload_count; slot++) {
                if (!variant->payload_names || !variant->payload_name_spans ||
                    !position_in_name(ctx->position, variant->payload_name_spans[slot],
                                      variant->payload_names[slot]))
                    continue;
                XlspEnumFieldIdentity identity = {
                    .enum_symbol_id = declaration->symbol_id,
                    .variant_ordinal = (uint16_t) ordinal,
                    .field_slot = (uint16_t) slot,
                };
                XrLocation location = {
                    .file = ctx->document_uri,
                    .line = (uint32_t) variant->payload_name_spans[slot].line,
                    .column = (uint32_t) variant->payload_name_spans[slot].column,
                };
                ctx->found = occurrence_from_identity(ctx->analyzer, identity, location, true,
                                                      ctx->out);
                return;
            }
        }
        return;
    }

    char **names = NULL;
    XrNameSpan *spans = NULL;
    int count = 0;
    if (node->type == AST_ENUM_CONSTRUCT) {
        names = node->as.enum_construct.field_names;
        spans = node->as.enum_construct.field_name_spans;
        count = node->as.enum_construct.field_count;
    } else if (node->type == AST_PATTERN_ADT) {
        names = node->as.pattern_adt.field_names;
        spans = node->as.pattern_adt.field_name_spans;
        count = node->as.pattern_adt.count;
    } else {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (!names || !spans || !position_in_name(ctx->position, spans[i], names[i]))
            continue;
        ctx->found = plan_occurrence(ctx->analyzer, node, ctx->document_uri, i, spans[i], ctx->out);
        return;
    }
}

bool xlsp_enum_field_at(XaAnalyzer *analyzer, AstNode *root, const char *document_uri,
                        XrLspPosition position, XlspEnumFieldOccurrence *out) {
    if (!analyzer || !root || !document_uri || !out)
        return false;
    FieldAtContext ctx = {
        .analyzer = analyzer,
        .document_uri = document_uri,
        .position = position,
        .out = out,
    };
    xa_ast_walk(root, find_field_at_node, NULL, &ctx);
    return ctx.found;
}

typedef struct FieldVisitContext {
    XaAnalyzer *analyzer;
    const char *document_uri;
    XlspEnumFieldIdentity target;
    XlspEnumFieldVisitFn visit;
    void *user_ctx;
} FieldVisitContext;

static void visit_matching_field(AstNode *node, void *raw_ctx) {
    FieldVisitContext *ctx = raw_ctx;
    if (!node || !ctx)
        return;

    if (node->type == AST_ENUM_DECL) {
        EnumDeclNode *declaration = &node->as.enum_decl;
        if (declaration->symbol_id != ctx->target.enum_symbol_id ||
            ctx->target.variant_ordinal >= declaration->member_count)
            return;
        AstNode *member = declaration->members[ctx->target.variant_ordinal];
        if (!member || member->type != AST_ENUM_MEMBER)
            return;
        EnumMemberNode *variant = &member->as.enum_member;
        if (ctx->target.field_slot >= variant->payload_count ||
            !variant->payload_name_spans)
            return;
        XrNameSpan span = variant->payload_name_spans[ctx->target.field_slot];
        XrLocation location = {
            .file = ctx->document_uri,
            .line = (uint32_t) span.line,
            .column = (uint32_t) span.column,
        };
        XlspEnumFieldOccurrence occurrence;
        if (occurrence_from_identity(ctx->analyzer, ctx->target, location, true, &occurrence))
            ctx->visit(&occurrence, ctx->user_ctx);
        return;
    }

    XrNameSpan *spans = NULL;
    int count = 0;
    if (node->type == AST_ENUM_CONSTRUCT) {
        spans = node->as.enum_construct.field_name_spans;
        count = node->as.enum_construct.field_count;
    } else if (node->type == AST_PATTERN_ADT) {
        spans = node->as.pattern_adt.field_name_spans;
        count = node->as.pattern_adt.count;
    } else {
        return;
    }

    const XaEnumRecordPlan *plan = xa_analyzer_get_enum_record_plan(ctx->analyzer, node);
    if (!plan || !plan->complete || plan->enum_symbol_id != ctx->target.enum_symbol_id ||
        plan->variant_ordinal != ctx->target.variant_ordinal || !plan->source_to_slot || !spans)
        return;
    for (int i = 0; i < count && i < plan->source_field_count; i++) {
        if (plan->source_to_slot[i] != ctx->target.field_slot)
            continue;
        XlspEnumFieldOccurrence occurrence;
        if (plan_occurrence(ctx->analyzer, node, ctx->document_uri, i, spans[i], &occurrence))
            ctx->visit(&occurrence, ctx->user_ctx);
    }
}

void xlsp_visit_enum_field_occurrences(XaAnalyzer *analyzer, AstNode *root,
                                       const char *document_uri,
                                       XlspEnumFieldIdentity target,
                                       XlspEnumFieldVisitFn visit, void *ctx) {
    if (!analyzer || !root || !document_uri || !visit ||
        !variant_for_identity(analyzer, target, NULL))
        return;
    FieldVisitContext visit_ctx = {
        .analyzer = analyzer,
        .document_uri = document_uri,
        .target = target,
        .visit = visit,
        .user_ctx = ctx,
    };
    xa_ast_walk(root, visit_matching_field, NULL, &visit_ctx);
}
