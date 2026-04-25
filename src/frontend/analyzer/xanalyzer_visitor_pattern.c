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
 *   `match expr { pattern => body, ... }` is the analyzer's most
 *   complex expression: it must (a) infer the union of all arm body
 *   types, (b) thread variable bindings through arm scopes, and
 *   (c) verify exhaustiveness for both enum subjects and `typeof()`
 *   subjects on union / nullable types. The supporting helpers
 *   (type_member_to_kind, kind_to_type_member, collect_matched_*
 *   pattern walkers) live alongside the visitor that uses them.
 *
 *   Phase 2.3 split this out of xanalyzer_visitor_expr.c. The
 *   implementation is unchanged from before the split.
 */

#include "xanalyzer_visitor_internal.h"
#include "../../base/xchecks.h"

// Forward declaration (defined below in narrowing section)
// Defined in xanalyzer_visitor_stmt.c

// Helper: map Type.xxx member name to XrTypeKind for typeof exhaustiveness
static XrTypeKind type_member_to_kind(const char *name) {
    if (!name) return XR_KIND_COUNT;
    if (strcmp(name, "int") == 0)      return XR_KIND_INT;
    if (strcmp(name, "float") == 0)    return XR_KIND_FLOAT;
    if (strcmp(name, "string") == 0)   return XR_KIND_STRING;
    if (strcmp(name, "bool") == 0)     return XR_KIND_BOOL;
    if (strcmp(name, "null") == 0)     return XR_KIND_NULL;
    if (strcmp(name, "Array") == 0)    return XR_KIND_ARRAY;
    if (strcmp(name, "Map") == 0)      return XR_KIND_MAP;
    if (strcmp(name, "Set") == 0)      return XR_KIND_SET;
    if (strcmp(name, "Json") == 0)     return XR_KIND_JSON;
    if (strcmp(name, "function") == 0) return XR_KIND_FUNCTION;
    if (strcmp(name, "Regex") == 0)    return XR_KIND_INSTANCE;
    if (strcmp(name, "BigInt") == 0)   return XR_KIND_INSTANCE;
    if (strcmp(name, "Channel") == 0)  return XR_KIND_CHANNEL;
    return XR_KIND_COUNT;
}

// Helper: get display name for XrTypeKind as Type.xxx
static const char *kind_to_type_member(XrTypeKind kind) {
    switch (kind) {
        case XR_KIND_INT:      return "Type.int";
        case XR_KIND_FLOAT:    return "Type.float";
        case XR_KIND_STRING:   return "Type.string";
        case XR_KIND_BOOL:     return "Type.bool";
        case XR_KIND_NULL:     return "Type.null";
        case XR_KIND_ARRAY:    return "Type.Array";
        case XR_KIND_MAP:      return "Type.Map";
        case XR_KIND_SET:      return "Type.Set";
        case XR_KIND_JSON:     return "Type.Json";
        case XR_KIND_FUNCTION: return "Type.function";
        case XR_KIND_CHANNEL:  return "Type.Channel";
        default:               return NULL;
    }
}

// Helper: collect Type.xxx member names from a match arm pattern
static void collect_matched_type_members(AstNode *pattern, XrTypeKind *kinds,
                                          int *count, int cap) {
    if (!pattern || *count >= cap) return;

    if (pattern->type == AST_PATTERN_LITERAL && pattern->as.pattern_literal.value) {
        AstNode *val = pattern->as.pattern_literal.value;
        if (val->type == AST_MEMBER_ACCESS && val->as.member_access.object &&
            val->as.member_access.object->type == AST_VARIABLE &&
            val->as.member_access.object->as.variable.name &&
            strcmp(val->as.member_access.object->as.variable.name, "Type") == 0) {
            XrTypeKind k = type_member_to_kind(val->as.member_access.name);
            if (k != XR_KIND_COUNT && *count < cap) {
                kinds[(*count)++] = k;
            }
        }
    } else if (pattern->type == AST_MEMBER_ACCESS && pattern->as.member_access.object &&
               pattern->as.member_access.object->type == AST_VARIABLE &&
               pattern->as.member_access.object->as.variable.name &&
               strcmp(pattern->as.member_access.object->as.variable.name, "Type") == 0) {
        XrTypeKind k = type_member_to_kind(pattern->as.member_access.name);
        if (k != XR_KIND_COUNT && *count < cap) {
            kinds[(*count)++] = k;
        }
    } else if (pattern->type == AST_PATTERN_MULTI) {
        PatternMultiNode *multi = &pattern->as.pattern_multi;
        for (int i = 0; i < multi->count; i++) {
            collect_matched_type_members(multi->patterns[i], kinds, count, cap);
        }
    }
}

// Helper: add enum member name to collection
static void add_enum_member(const char *member, const char ***names, int *count, int *cap) {
    if (!member) return;
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *names = xr_realloc(*names, sizeof(const char*) * (*cap));
    }
    (*names)[(*count)++] = member;
}

// Helper: collect enum member names from a match arm pattern
static void collect_matched_enum_members(AstNode *pattern, const char ***names,
                                          int *count, int *cap) {
    if (!pattern) return;

    if (pattern->type == AST_ENUM_ACCESS) {
        add_enum_member(pattern->as.enum_access.member_name, names, count, cap);
    } else if (pattern->type == AST_MEMBER_ACCESS) {
        add_enum_member(pattern->as.member_access.name, names, count, cap);
    } else if (pattern->type == AST_PATTERN_LITERAL && pattern->as.pattern_literal.value) {
        // Unwrap: AST_PATTERN_LITERAL wrapping AST_MEMBER_ACCESS or AST_ENUM_ACCESS
        collect_matched_enum_members(pattern->as.pattern_literal.value, names, count, cap);
    } else if (pattern->type == AST_PATTERN_MULTI) {
        PatternMultiNode *multi = &pattern->as.pattern_multi;
        for (int i = 0; i < multi->count; i++) {
            collect_matched_enum_members(multi->patterns[i], names, count, cap);
        }
    }
}

XrType *xa_visit_match_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown(NULL);

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
        if (!arm || arm->type != AST_MATCH_ARM) continue;

        MatchArmNode *arm_node = &arm->as.match_arm;

        // Check for wildcard pattern
        if (arm_node->pattern && arm_node->pattern->type == AST_PATTERN_WILDCARD) {
            has_wildcard = true;
        }

        // Register binding variable if pattern is a variable capture (e.g. n if (n < 0) => ...)
        bool has_binding = false;
        if (arm_node->pattern && arm_node->pattern->type == AST_PATTERN_LITERAL) {
            AstNode *pval = arm_node->pattern->as.pattern_literal.value;
            if (pval && pval->type == AST_VARIABLE) {
                has_binding = true;
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, arm);
                XaSymbol *bind_sym = xa_symbol_new(pval->as.variable.name, XA_SYM_VARIABLE);
                bind_sym->location.line = pval->line;
                xa_scope_add_symbol(ctx->analyzer->current_scope, bind_sym);
                XaSymbolLinks *bind_links = xa_analyzer_get_links(ctx->analyzer, bind_sym);
                if (bind_links) {
                    bind_links->type = subject_type ? subject_type : xr_type_new_unknown(NULL);
                    bind_links->is_definitely_assigned = true;
                }
            }
        }

        // Infer guard if present
        if (arm_node->guard) {
            xa_visit_infer_expr(ctx, arm_node->guard);
        }

        // Infer body type
        XrType *arm_type = xr_type_new_unknown(NULL);
        if (arm_node->body) {
            arm_type = xa_visit_infer_expr(ctx, arm_node->body);
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
    if (!has_wildcard && !XR_TYPE_IS_ENUM(subject_type) && match->expr && match->expr->type == AST_VARIABLE) {
        const char *var_name = match->expr->as.variable.name;
        XaSymbol *var_sym = xa_scope_lookup(ctx->analyzer->current_scope, var_name);
        if (var_sym) {
            XaSymbolLinks *var_links = xa_analyzer_get_links(ctx->analyzer, var_sym);
            XrType *dt = var_links ? var_links->declared_type : NULL;
            if (dt) {
                if (XR_TYPE_IS_ENUM(dt)) {
                    subject_type = dt;
                } else if (XR_TYPE_IS_CLASS(dt) && dt->instance.class_name) {
                    // Parser resolves enum names as XR_KIND_CLASS; check symbol table
                    XaSymbol *maybe_enum = xa_scope_lookup(ctx->analyzer->current_scope, dt->instance.class_name);
                    if (maybe_enum && maybe_enum->kind == XA_SYM_ENUM) {
                        XaSymbolLinks *el = xa_analyzer_get_links(ctx->analyzer, maybe_enum);
                        if (el && el->type && XR_TYPE_IS_ENUM(el->type)) {
                            subject_type = el->type;
                        }
                    }
                }
            }
        }
    }
    if (subject_type && XR_TYPE_IS_ENUM(subject_type) && !has_wildcard) {
        const char *enum_name = subject_type->enum_type.enum_name;
        if (enum_name) {
            XaSymbol *enum_sym = xa_scope_lookup(ctx->analyzer->current_scope, enum_name);
            if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
                XaSymbolLinks *enum_links = xa_analyzer_get_links(ctx->analyzer, enum_sym);

                if (enum_links && enum_links->enum_member_count > 0) {
                    // Collect matched members from all arms
                    const char **matched = NULL;
                    int matched_count = 0, matched_cap = 0;

                    for (int i = 0; i < match->arm_count; i++) {
                        AstNode *arm = match->arms[i];
                        if (!arm || arm->type != AST_MATCH_ARM) continue;
                        collect_matched_enum_members(arm->as.match_arm.pattern,
                                                     &matched, &matched_count, &matched_cap);
                    }

                    // Check which enum members are missing
                    for (int i = 0; i < enum_links->enum_member_count; i++) {
                        const char *member_name = enum_links->enum_member_names[i];
                        if (!member_name) continue;

                        bool found = false;
                        for (int j = 0; j < matched_count; j++) {
                            if (strcmp(matched[j], member_name) == 0) {
                                found = true;
                                break;
                            }
                        }

                        if (!found) {
                            XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                "Non-exhaustive match: missing '%s.%s'", enum_name, member_name);
                            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        }
                    }

                    if (matched) xr_free(matched);
                }
            }
        }
    }

    // Exhaustiveness check for typeof match on union/nullable types
    // Detects: match typeof(x) { Type.int => ..., Type.string => ... }
    if (!has_wildcard && match->expr) {
        const char *typeof_var = get_typeof_arg_name(match->expr);
        if (typeof_var) {
            // Look up the static type of the variable
            XaSymbol *var_sym = xa_scope_lookup(ctx->analyzer->current_scope, typeof_var);
            XrType *var_type = NULL;
            if (var_sym) {
                XaSymbolLinks *var_links = xa_analyzer_get_links(ctx->analyzer, var_sym);
                if (var_links) var_type = var_links->declared_type ? var_links->declared_type : var_links->type;
            }

            if (var_type) {
                // Build expected type kind set from the variable's static type
                XrTypeKind expected[XR_UNION_MAX_MEMBERS + 1];
                int expected_count = 0;

                if (XR_TYPE_IS_UNION(var_type)) {
                    for (int i = 0; i < var_type->union_type.member_count && expected_count < XR_UNION_MAX_MEMBERS; i++) {
                        XrType *m = var_type->union_type.members[i];
                        if (m) expected[expected_count++] = m->kind;
                    }
                } else if (var_type->is_nullable) {
                    // T? means we need T and null
                    XrType *base = xr_type_non_nullable(ctx->analyzer->isolate, var_type);
                    if (base) expected[expected_count++] = base->kind;
                    expected[expected_count++] = XR_KIND_NULL;
                }

                if (expected_count > 0) {
                    // Collect matched Type.xxx kinds from arms
                    XrTypeKind matched_kinds[32];
                    int matched_count = 0;

                    for (int i = 0; i < match->arm_count; i++) {
                        AstNode *arm = match->arms[i];
                        if (!arm || arm->type != AST_MATCH_ARM) continue;
                        collect_matched_type_members(arm->as.match_arm.pattern,
                                                     matched_kinds, &matched_count, 32);
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
                                XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                    "Non-exhaustive match: missing '%s' for type '%s'",
                                    missing, xr_type_to_string(var_type));
                                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                    XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                            }
                        }
                    }
                }
            }
        }
    }

    return result ? result : xr_type_new_unknown(NULL);
}
