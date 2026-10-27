/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_constraints.inc.c - Declaration-scoped interface applications
 */
static SourceName *source_interface_name(SourceContext *ctx, AstNode *node, const char *name,
    uint32_t *binding) {
    if (!name || !*name) return NULL;
    int count = 0;
    XrGenericParam **parameters = source_type_parameters(ctx, &count);
    for (int p = 0; p < count; ++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return NULL;
        if (source_text_same(ctx, NULL, parameters[p]->name, name)) {
            source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface requirement names a type parameter"); return NULL;
        }
    }
    const char *dot = source_text_find(ctx, name, '.');
    SourceName *symbol = NULL;
    if (dot) {
        size_t length = (size_t)(dot - name);
        char *prefix = source_alloc(ctx, length + 1, 1);
        if (!prefix) return NULL;
        if (!source_copy_bytes(ctx, node, prefix, name, length)) return NULL;
        symbol = visible_name(ctx, prefix);
        if (symbol && symbol->kind == SOURCE_MODULE) symbol = imported_declaration(ctx, symbol, dot + 1);
        else symbol = NULL;
        if (symbol) *binding = symbol->declaration;
    } else {
        symbol = visible_name(ctx, name);
        if (symbol) *binding = symbol->declaration;
        if (symbol && symbol->kind == SOURCE_IMPORT) symbol = imported_declaration(ctx, symbol, symbol->imported);
    }
    return symbol && symbol->kind == SOURCE_INTERFACE ? symbol : NULL;
}
static bool source_interface_reference(SourceContext *ctx, const XrTypeRef *ref,
    uint32_t binding, uint32_t target) {
    XrTypeNameSpan span = ref->name_span;
    if (!span.line && !span.column && !span.length) return true;
    if (span.line <= 0 || span.column <= 0 || span.length <= 0 || span.length > INT_MAX - span.column ||
        !binding || !target)
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "interface reference token is invalid");
    XrXirSourceRange range = {ctx->module, span.line, span.column, span.line, span.column + span.length};
    /* Constraints can be checked more than once. One written token keeps one
     * binding/target observation, independently of semantic replay. */
    for (uint32_t i = 0; i < ctx->query.reference_count; ++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirSourceReference *existing = &ctx->query.references[i];
        if (existing->range.module == range.module && existing->range.line == range.line &&
            existing->range.column == range.column && existing->range.end_line == range.end_line &&
            existing->range.end_column == range.end_column && existing->declaration == binding &&
            existing->target == target && existing->access == XR_XIR_SOURCE_TYPE_USE) return true;
    }
    XrXirSourceReference *records = source_query_append(ctx, ctx->query.references,
        &ctx->query.reference_count, &ctx->reference_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.references = records;
    records[ctx->query.reference_count - 1] = (XrXirSourceReference) {range, binding, target, XR_XIR_SOURCE_TYPE_USE};
    return source_query_syntax_reference(ctx, range);
}
static bool source_interface_application(SourceContext *ctx, AstNode *node,
    const XrTypeRef *ref, XrXirInterfaceApplication *output) {
    if (!ref || (ref->kind != XR_TREF_NAMED && ref->kind != XR_TREF_GENERIC) ||
        !ref->name || (ref->kind == XR_TREF_NAMED && ref->nchildren) ||
        (ref->nchildren && !ref->children))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface application requires an explicit named interface");
    uint32_t binding = 0;
    SourceName *symbol = source_interface_name(ctx, node, ref->name, &binding);
    if (!symbol) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constraint name does not resolve to an interface");
    const XrXirInterfaceDeclaration *declaration = &ctx->interfaces.declarations[symbol->index];
    uint32_t index = symbol->index, target = symbol->declaration;
    uint32_t count = ref->nchildren;
    if (count != declaration->parameter_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface application requires its exact explicit arguments");
    XrXirType *arguments = count ? source_alloc(ctx, count, sizeof(*arguments)) : NULL;
    if (count && !arguments) return false;
    for (uint32_t a = 0; a < count; ++a) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_type(ctx, ref->children[a], &arguments[a])) return false;
        if (arguments[a] == XR_XIR_UNIT || xr_xir_type_is_cell(&ctx->types, arguments[a]))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface argument must be an admitted value type");
    }
    *output = (XrXirInterfaceApplication){index, arguments, count};
    return source_interface_reference(ctx, ref, binding, target);
}
static bool source_constraint_same(SourceContext *ctx,
    const XrXirInterfaceApplication *a, const XrXirInterfaceApplication *b, bool *same) {
    *same = false;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (a->declaration != b->declaration || a->argument_count != b->argument_count) return true;
    for (uint32_t p = 0; p < a->argument_count; ++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (a->arguments[p] != b->arguments[p]) return true;
    }
    *same = true; return true;
}
/* Predicate spellings come from the registry. Ordinary visible declarations
 * and parameter bindings win before the builtin category is considered. */
static uint32_t source_builtin_hashable_marker(SourceContext *ctx, const char *name, uint32_t arity) {
    if (arity) return 0;
    return source_text_same(ctx, NULL, name, "Hashable") ? XR_XIR_CONSTRAINT_HASHABLE : 0;
}
static uint32_t source_predicate_marker(SourceContext *ctx, const char *name) {
    if (!name || visible_name(ctx,name)) return 0;
    int count = 0;
    XrGenericParam **parameters = source_type_parameters(ctx,&count);
    for (int p = 0; p < count; ++p)
        if (source_text_same(ctx, NULL, parameters[p]->name, name)) return 0;
#define XR_XIR_PREDICATE_VALUE_EQUAL XR_XIR_CONSTRAINT_EQUAL
#define XR_XIR_PREDICATE_ATOMIC_VALUE XR_XIR_CONSTRAINT_ATOMIC_VALUE
#define XR_XIR_PREDICATE_ATOMIC_NUMBER XR_XIR_CONSTRAINT_ATOMIC_NUMBER
#define XR_XIR_PREDICATE_ATOMIC_BOOLEAN XR_XIR_CONSTRAINT_ATOMIC_BOOLEAN
#define XR_BUILTIN_PREDICATE(spelling, arity, identity) \
    if (source_text_same(ctx, NULL, name, spelling)) return XR_XIR_PREDICATE_##identity;
#define XR_BUILTIN_IFACE(spelling, arity) \
    if (source_text_same(ctx, NULL, name, spelling)) return source_builtin_hashable_marker(ctx, spelling, arity);
#include "../../stdlib/prelude/builtin_symbols.def"
#undef XR_XIR_PREDICATE_VALUE_EQUAL
#undef XR_XIR_PREDICATE_ATOMIC_VALUE
#undef XR_XIR_PREDICATE_ATOMIC_NUMBER
#undef XR_XIR_PREDICATE_ATOMIC_BOOLEAN
    return 0;
}
static bool source_parameter_constraints(SourceContext *ctx, AstNode *node,
    const XrGenericParam *parameter, XrXirConstraint *output) {
    if (!parameter || !parameter->name || !*parameter->name ||
        source_text_same(ctx, NULL, parameter->name, "Sendable") || source_text_same(ctx, NULL, parameter->name, "Error") ||
        parameter->constraint_count < 0 || parameter->constraint_count > 65536 ||
        (parameter->constraint_count && !parameter->constraints))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "generic parameter contract is not admitted");
    XrXirConstraint result = {0};
    XrXirInterfaceApplication *applications = parameter->constraint_count ?
        source_alloc(ctx, (uint32_t)parameter->constraint_count, sizeof(*applications)) : NULL;
    if (parameter->constraint_count && !applications) return false;
    for (int i = 0; i < parameter->constraint_count; ++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrTypeRef *ref = parameter->constraints[i];
        if (ref && ref->kind == XR_TREF_NAMED && !ref->nchildren && ref->name) {
            uint32_t marker = source_text_same(ctx, NULL, ref->name, "Sendable") ? XR_XIR_CONSTRAINT_SENDABLE :
                source_text_same(ctx, NULL, ref->name, "Error") ? XR_XIR_CONSTRAINT_ERROR : source_predicate_marker(ctx,ref->name);
            if (marker) { result.markers |= marker; continue; }
        }
        XrXirInterfaceApplication application = {0};
        if (!source_interface_application(ctx, node, ref, &application)) return false;
        bool duplicate = false;
        for (uint32_t a = 0; a < result.interface_count; ++a) {
            bool same;
            if (!source_constraint_same(ctx, &applications[a], &application, &same)) return false;
            if (same) { duplicate = true; break; }
        }
        if (!duplicate) applications[result.interface_count++] = application;
    }
    result.interfaces = result.interface_count ? applications : NULL;
    *output = result; return true;
}
static bool source_constraint_conjunction(SourceContext *ctx, AstNode *node,
    XrXirConstraint left, XrXirConstraint right, XrXirConstraint *output) {
    if (right.interface_count > UINT32_MAX - left.interface_count)
        return source_fail(ctx,node,XR_XIR_BUDGET,"constraint application count exhausted");
    uint32_t capacity = left.interface_count + right.interface_count;
    XrXirInterfaceApplication *applications = capacity ? source_alloc(ctx,capacity,sizeof(*applications)) : NULL;
    if (capacity && !applications) return false;
    XrXirConstraint result = {0}; result.markers = left.markers | right.markers;
    for (uint32_t i = 0; i < capacity; ++i) {
        const XrXirInterfaceApplication *application = i < left.interface_count ?
            &left.interfaces[i] : &right.interfaces[i - left.interface_count];
        bool duplicate = false;
        for (uint32_t p = 0; p < result.interface_count; ++p) {
            bool same;
            if (!source_constraint_same(ctx,&applications[p],application,&same)) return false;
            if (same) { duplicate = true; break; }
        }
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!duplicate) applications[result.interface_count++] = *application;
    }
    result.interfaces = result.interface_count ? applications : NULL;
    *output = result; return true;
}
