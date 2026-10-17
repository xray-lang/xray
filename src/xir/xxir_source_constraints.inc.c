/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_constraints.inc.c - Declaration-scoped interface applications
 */
static SourceName *source_interface_name(SourceContext *ctx, AstNode *node, const char *name) {
    if (!name || !*name) return NULL;
    int count = 0;
    XrGenericParam **parameters = source_type_parameters(ctx, &count);
    for (int p = 0; p < count; ++p) {
        if (!source_work(ctx, node)) return NULL;
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
    } else {
        symbol = visible_name(ctx, name);
        if (symbol && symbol->kind == SOURCE_IMPORT) symbol = imported_declaration(ctx, symbol, symbol->imported);
    }
    return symbol && symbol->kind == SOURCE_INTERFACE ? symbol : NULL;
}
static bool source_interface_application(SourceContext *ctx, AstNode *node,
    const XrTypeRef *ref, XrXirInterfaceApplication *output) {
    if (!ref || (ref->kind != XR_TREF_NAMED && ref->kind != XR_TREF_GENERIC) ||
        !ref->name || (ref->kind == XR_TREF_NAMED && ref->nchildren) ||
        (ref->nchildren && !ref->children))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface application requires an explicit named interface");
    SourceName *symbol = source_interface_name(ctx, node, ref->name);
    if (!symbol) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constraint name does not resolve to an interface");
    const XrXirInterfaceDeclaration *declaration = &ctx->interfaces.declarations[symbol->index];
    uint32_t count = ref->nchildren;
    if (count != declaration->parameter_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface application requires its exact explicit arguments");
    XrXirType *arguments = count ? source_alloc(ctx, count, sizeof(*arguments)) : NULL;
    if (count && !arguments) return false;
    for (uint32_t a = 0; a < count; ++a) {
        if (!source_work(ctx, node) || !source_type(ctx, ref->children[a], &arguments[a])) return false;
        if (arguments[a] == XR_XIR_UNIT || xr_xir_type_is_cell(&ctx->types, arguments[a]))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface argument must be an admitted value type");
    }
    *output = (XrXirInterfaceApplication){symbol->index, arguments, count};
    return true;
}
static bool source_constraint_same(SourceContext *ctx, AstNode *node,
    const XrXirInterfaceApplication *a, const XrXirInterfaceApplication *b, bool *same) {
    *same = false;
    if (!source_work(ctx, node)) return false;
    if (a->declaration != b->declaration || a->argument_count != b->argument_count) return true;
    for (uint32_t p = 0; p < a->argument_count; ++p) {
        if (!source_work(ctx, node)) return false;
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
static bool source_structural_append(SourceContext *ctx,AstNode *node,
    XrXirStructuralRequirement *requirements,uint32_t *count,XrXirStructuralRequirement requirement) {
    for (uint32_t r=0;r<*count;++r) {
        if (!source_work(ctx,node)) return false;
        if (requirements[r].shape==requirement.shape && requirements[r].policy==requirement.policy) return true;
    }
    if (!source_work(ctx,node)) return false;
    requirements[(*count)++]=requirement;return true;
}
static bool source_structural_constraints_check(SourceContext *ctx,AstNode *node,XrXirConstraint value) {
    if (!value.structural_count) return true;
    value.markers=0;value.interfaces=NULL;value.interface_count=0;
    int count=0;(void)source_type_parameters(ctx,&count);
    XrXirStatus status=xr_xir_compile_constraint_structure(&ctx->compile,&ctx->types,value,(uint32_t)count);
    return status==XR_XIR_OK || source_fail(ctx,node,status,"structural fields require exact types and readonly receiver origins");
}
static bool source_parameter_constraints(SourceContext *ctx, AstNode *node,
    const XrGenericParam *parameter, XrXirConstraint *output) {
    if (!parameter || !parameter->name || !*parameter->name ||
        source_text_same(ctx, NULL, parameter->name, "Sendable") || source_text_same(ctx, NULL, parameter->name, "Error") ||
        parameter->constraint_count < 0 || parameter->constraint_count > 65536 ||
        (parameter->constraint_count && !parameter->constraints))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "generic parameter contract is not admitted");
    XrXirConstraint result = {0};
    XrXirStructuralRequirement *requirements=NULL;
    XrXirInterfaceApplication *applications = parameter->constraint_count ?
        source_alloc(ctx, (uint32_t)parameter->constraint_count, sizeof(*applications)) : NULL;
    if (parameter->constraint_count && !applications) return false;
    for (int i = 0; i < parameter->constraint_count; ++i) {
        if (!source_work(ctx, node)) return false;
        const XrTypeRef *ref = parameter->constraints[i];
        if (ref && ref->kind == XR_TREF_NAMED && !ref->nchildren && ref->name) {
            uint32_t marker = source_text_same(ctx, NULL, ref->name, "Sendable") ? XR_XIR_CONSTRAINT_SENDABLE :
                source_text_same(ctx, NULL, ref->name, "Error") ? XR_XIR_CONSTRAINT_ERROR : source_predicate_marker(ctx,ref->name);
            if (marker) { result.markers |= marker; continue; }
        }
        SourceName *interface=ref && (ref->kind==XR_TREF_NAMED || ref->kind==XR_TREF_GENERIC) ?
            source_interface_name(ctx,node,ref->name) : NULL;
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (!interface) {
            XrXirType shape;
            if (!source_type(ctx,(XrTypeRef *)ref,&shape)) return false;
            if (!xr_xir_type_is_record(&ctx->types,shape))
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"structural constraints require an anonymous Record shape");
            if (!requirements) requirements=source_alloc(ctx,(uint32_t)parameter->constraint_count,sizeof(*requirements));
            if (!requirements || !source_structural_append(ctx,node,requirements,&result.structural_count,
                (XrXirStructuralRequirement){shape,XR_XIR_STRUCTURAL_READONLY_RECEIVER})) return false;
            continue;
        }
        XrXirInterfaceApplication application = {0};
        if (!source_interface_application(ctx, node, ref, &application)) return false;
        bool duplicate = false;
        for (uint32_t a = 0; a < result.interface_count; ++a) {
            bool same;
            if (!source_constraint_same(ctx, node, &applications[a], &application, &same)) return false;
            if (same) { duplicate = true; break; }
        }
        if (!duplicate) applications[result.interface_count++] = application;
    }
    result.interfaces = result.interface_count ? applications : NULL;
    result.structural_requirements=result.structural_count ? requirements : NULL;
    if (!source_structural_constraints_check(ctx,node,result)) return false;
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
            if (!source_constraint_same(ctx,node,&applications[p],application,&same)) return false;
            if (same) { duplicate = true; break; }
        }
        if (!source_work(ctx,node)) return false;
        if (!duplicate) applications[result.interface_count++] = *application;
    }
    result.interfaces = result.interface_count ? applications : NULL;
    if (right.structural_count>UINT32_MAX-left.structural_count)
        return source_fail(ctx,node,XR_XIR_BUDGET,"structural requirement count exhausted");
    uint32_t structures=left.structural_count+right.structural_count;
    XrXirStructuralRequirement *requirements=structures ? source_alloc(ctx,structures,sizeof(*requirements)) : NULL;
    if (structures && !requirements) return false;
    for (uint32_t r=0;r<structures;++r) {
        XrXirStructuralRequirement requirement=r<left.structural_count ? left.structural_requirements[r] :
            right.structural_requirements[r-left.structural_count];
        if (!source_structural_append(ctx,node,requirements,&result.structural_count,requirement)) return false;
    }
    result.structural_requirements=result.structural_count ? requirements : NULL;
    if (!source_structural_constraints_check(ctx,node,result)) return false;
    *output = result; return true;
}
