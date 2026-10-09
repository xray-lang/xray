/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_domain.inc.c - Exact declaration domains across pools
 *
 * KEY CONCEPT:
 *   Equal numeric nominal and interface ordinals prove nothing on their own.
 *   Ordered full declarations and every structural child must agree first.
 */
#include "xxir_constraints.h"

static XrXirStatus effect_terms_literal_same(EffectTerms *pool, XrXirLiteral a, XrXirLiteral b) {
    if (!xir_compile_work(pool->remaining,1)) return XR_XIR_BUDGET;
    if (a.length!=b.length || (a.length && (!a.bytes || !b.bytes))) return XR_XIR_BAD_TYPE;
    if (!xir_compile_work(pool->remaining,a.length)) return XR_XIR_BUDGET;
    return !a.length || !memcmp(a.bytes,b.bytes,a.length) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}

static XrXirStatus effect_terms_domain_type(EffectTerms *pool, const XrXirTypes *source,
    XrXirType expected, XrXirType actual, uint32_t scope) {
    XrXirType *arguments=scope ? effect_terms_alloc(pool,scope,sizeof(*arguments)) : NULL;
    if (scope && !arguments) return pool->status;
    for (uint32_t p=0;p<scope;++p) {
        if (!xir_compile_work(pool->remaining,sizeof(*arguments)+1)) return XR_XIR_BUDGET;
        arguments[p]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    }
    if (!pool->match.resources) pool->match.resources=pool->remaining->resources;
    return xr_xir_compile_type_substitution_matches_between_scratch(pool->remaining,
        &pool->types,source,arguments,scope,expected,actual,&pool->match);
}

static XrXirStatus effect_terms_domain_constraints(EffectTerms *pool, const XrXirTypes *source,
    const XrXirConstraint *expected, const XrXirConstraint *actual, uint32_t count) {
    for (uint32_t p=0;p<count;++p) {
        if (!expected || !actual) return XR_XIR_BAD_TYPE;
        XrXirStatus status=xr_xir_compile_constraint_records_match(pool->remaining,
            &pool->types,expected[p],source,actual[p],count);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_terms_nominal_head(EffectTerms *pool,
    const XrXirNominalDeclaration *a, const XrXirNominalDeclaration *b) {
    if (!xir_compile_work(pool->remaining,8+sizeof(a->native))) return XR_XIR_BUDGET;
    if (a->exported!=b->exported || a->parameter_count!=b->parameter_count ||
        a->field_count!=b->field_count || a->kind!=b->kind ||
        a->variant_count!=b->variant_count || a->flags!=b->flags ||
        a->native.native_id!=b->native.native_id ||
        memcmp(a->native.source_fingerprint,b->native.source_fingerprint,32)) return XR_XIR_BAD_TYPE;
    XrXirStatus status=effect_terms_literal_same(pool,a->module,b->module);
    if (status==XR_XIR_OK) status=effect_terms_literal_same(pool,a->name,b->name);
    for (uint32_t f=0;f<a->field_count && status==XR_XIR_OK;++f) {
        if (!a->fields || !b->fields || a->fields[f].flags!=b->fields[f].flags) return XR_XIR_BAD_TYPE;
        status=effect_terms_literal_same(pool,a->fields[f].name,b->fields[f].name);
    }
    for (uint32_t v=0;v<a->variant_count && status==XR_XIR_OK;++v) {
        if (!a->variants || !b->variants || a->variants[v].field_begin!=b->variants[v].field_begin ||
            a->variants[v].field_count!=b->variants[v].field_count) return XR_XIR_BAD_TYPE;
        status=effect_terms_literal_same(pool,a->variants[v].name,b->variants[v].name);
    }
    return status;
}

static XrXirStatus effect_terms_interface_head(EffectTerms *pool,
    const XrXirInterfaceDeclaration *a, const XrXirInterfaceDeclaration *b) {
    if (!xir_compile_work(pool->remaining,4)) return XR_XIR_BUDGET;
    if (a->exported!=b->exported || a->parameter_count!=b->parameter_count ||
        a->parent_count!=b->parent_count || a->method_count!=b->method_count) return XR_XIR_BAD_TYPE;
    XrXirStatus status=effect_terms_literal_same(pool,a->module,b->module);
    if (status==XR_XIR_OK) status=effect_terms_literal_same(pool,a->name,b->name);
    for (uint32_t m=0;m<a->method_count && status==XR_XIR_OK;++m) {
        if (!a->methods || !b->methods || a->methods[m].receiver!=b->methods[m].receiver ||
            a->methods[m].own_parameter_count!=b->methods[m].own_parameter_count) return XR_XIR_BAD_TYPE;
        status=effect_terms_literal_same(pool,a->methods[m].name,b->methods[m].name);
    }
    return status;
}

static XrXirStatus effect_terms_domain_heads(EffectTerms *pool, const XrXirTypes *source) {
    const XrXirNominalTable *a=pool->types.nominals,*b=source->nominals;
    const XrXirInterfaceTable *x=pool->types.interfaces,*y=source->interfaces;
    if (!!a!=!!b || !!x!=!!y || (a && a->count!=b->count) || (x && x->count!=y->count))
        return XR_XIR_BAD_TYPE;
    if (a && (!a->declarations || !b->declarations || a->identities || b->identities))
        return XR_XIR_BAD_TYPE;
    for (uint32_t n=0;a && n<a->count;++n) {
        XrXirStatus status=effect_terms_nominal_head(pool,&a->declarations[n],&b->declarations[n]);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t i=0;x && i<x->count;++i) {
        if (!x->declarations || !y->declarations) return XR_XIR_BAD_TYPE;
        XrXirStatus status=effect_terms_interface_head(pool,&x->declarations[i],&y->declarations[i]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Closed contextual inputs retain the original verified declaration order.
 * Projected Lowered identity-only tables cannot satisfy this semantic domain. */
static XrXirStatus effect_terms_domain_match(EffectTerms *pool, const XrXirTypes *source) {
    if (!xir_compile_work(pool->remaining,1)) return XR_XIR_BUDGET;
    if (pool->types.nominals==source->nominals && pool->types.interfaces==source->interfaces)
        return XR_XIR_OK;
    XrXirStatus status=effect_terms_domain_heads(pool,source);
    if (status!=XR_XIR_OK) return status;
    const XrXirNominalTable *nominals=pool->types.nominals;
    for (uint32_t n=0;nominals && n<nominals->count && status==XR_XIR_OK;++n) {
        const XrXirNominalDeclaration *a=&nominals->declarations[n],*b=&source->nominals->declarations[n];
        status=effect_terms_domain_constraints(pool,source,a->constraints,b->constraints,a->parameter_count);
        for (uint32_t f=0;f<a->field_count && status==XR_XIR_OK;++f)
            status=effect_terms_domain_type(pool,source,a->fields[f].type,b->fields[f].type,a->parameter_count);
    }
    const XrXirInterfaceTable *interfaces=pool->types.interfaces;
    for (uint32_t i=0;interfaces && i<interfaces->count && status==XR_XIR_OK;++i) {
        const XrXirInterfaceDeclaration *a=&interfaces->declarations[i],*b=&source->interfaces->declarations[i];
        status=effect_terms_domain_constraints(pool,source,a->constraints,b->constraints,a->parameter_count);
        XrXirConstraint parents_a={0,a->parents,a->parent_count},parents_b={0,b->parents,b->parent_count};
        if (status==XR_XIR_OK) status=xr_xir_compile_constraint_records_match(pool->remaining,
            &pool->types,parents_a,source,parents_b,a->parameter_count);
        for (uint32_t m=0;m<a->method_count && status==XR_XIR_OK;++m) {
            const XrXirInterfaceMethod *left=&a->methods[m],*right=&b->methods[m];
            uint32_t total=a->parameter_count+left->own_parameter_count;
            status=effect_terms_domain_type(pool,source,left->signature,right->signature,total);
            if (status==XR_XIR_OK && left->own_parameter_count) {
                XrXirConstraint *a_constraints=effect_terms_alloc(pool,total,sizeof(*a_constraints));
                XrXirConstraint *b_constraints=effect_terms_alloc(pool,total,sizeof(*b_constraints));
                if (!a_constraints || !b_constraints) return pool->status;
                for (uint32_t p=0;p<left->own_parameter_count;++p) {
                    if (!left->constraints || !right->constraints) return XR_XIR_BAD_TYPE;
                    if (!xir_compile_work(pool->remaining,2*sizeof(*a_constraints))) return XR_XIR_BUDGET;
                    a_constraints[a->parameter_count+p]=left->constraints[p];
                    b_constraints[a->parameter_count+p]=right->constraints[p];
                }
                status=effect_terms_domain_constraints(pool,source,a_constraints,b_constraints,total);
            }
        }
    }
    return status;
}

/* A Source type-pool change invalidates the whole context generation, including
 * private appended types and every cause certificate; numeric overlap is not a
 * reason to retain the old owner. */
static XrXirStatus effect_terms_snapshot_match(EffectTerms *pool,
    const XrXirTypes *source, uint32_t original_count) {
    if (!source || source->count!=original_count || original_count>pool->types.count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_terms_domain_match(pool,source);
    for (uint32_t t=0;t<original_count && status==XR_XIR_OK;++t) {
        const XrXirTypeNode *owned=&pool->types.nodes[t],*current=&source->nodes[t];
        if (owned->parameter_span!=current->parameter_span || !effect_term_same(pool,owned,current))
            status=pool->status==XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : pool->status;
    }
    return status==XR_XIR_BAD_TYPE ? XR_XIR_BAD_STRUCTURE : status;
}

/* Projection erases semantic constraints, never nominal identity or payload.
 * Real constraint and implementation proofs use the retained semantic owner. */
static XrXirStatus effect_terms_projected_domain(EffectTerms *pool, const XrXirTypes *actual) {
    const XrXirNominalTable *source=pool->types.nominals,*target=actual->nominals;
    if (actual->interfaces || !!source!=!!target || (source && source->count!=target->count))
        return XR_XIR_BAD_TYPE;
    uint32_t count=source ? source->count : 0;
    uint32_t *mapping=count ? effect_terms_alloc(pool,count,sizeof(*mapping)) : NULL;
    if (count && !mapping) return pool->status;
    for (uint32_t d=0;d<count;++d) {
        if (!xir_compile_work(pool->remaining,1)) return XR_XIR_BUDGET;
        mapping[d]=d;
        const XrXirNominalDeclaration *original=&source->declarations[d];
        XrXirStatus status=XR_XIR_OK;
        if (target->declarations) {
            const XrXirNominalDeclaration *projected=&target->declarations[d];
            status=effect_terms_nominal_head(pool,original,projected);
            for (uint32_t p=0;p<original->parameter_count && status==XR_XIR_OK;++p) {
                if (!original->constraints || !projected->constraints ||
                    original->constraints[p].markers!=projected->constraints[p].markers ||
                    projected->constraints[p].interfaces || projected->constraints[p].interface_count)
                    return XR_XIR_BAD_TYPE;
            }
            for (uint32_t f=0;f<original->field_count && status==XR_XIR_OK;++f)
                status=effect_terms_domain_type(pool,actual,original->fields[f].type,
                    projected->fields[f].type,original->parameter_count);
        } else {
            if (!target->identities) return XR_XIR_BAD_TYPE;
            const XrXirNominalIdentity *identity=&target->identities[d];
            if (!xir_compile_work(pool->remaining,sizeof(identity->native)+7)) return XR_XIR_BUDGET;
            if (original->exported!=identity->exported || original->parameter_count!=identity->arity ||
                original->field_count!=identity->field_count || original->kind!=identity->kind ||
                original->variant_count!=identity->variant_count || original->flags!=identity->flags ||
                original->native.native_id!=identity->native.native_id ||
                memcmp(original->native.source_fingerprint,identity->native.source_fingerprint,32))
                return XR_XIR_BAD_TYPE;
            status=effect_terms_literal_same(pool,original->module,identity->module);
            if (status==XR_XIR_OK) status=effect_terms_literal_same(pool,original->name,identity->name);
            for (uint32_t f=0;f<original->field_count && status==XR_XIR_OK;++f) {
                if (!identity->fields || original->fields[f].flags!=identity->fields[f].flags)
                    return XR_XIR_BAD_TYPE;
                status=effect_terms_literal_same(pool,original->fields[f].name,identity->fields[f].name);
            }
            for (uint32_t v=0;v<original->variant_count && status==XR_XIR_OK;++v) {
                if (!identity->variants || original->variants[v].field_begin!=identity->variants[v].field_begin ||
                    original->variants[v].field_count!=identity->variants[v].field_count) return XR_XIR_BAD_TYPE;
                status=effect_terms_literal_same(pool,original->variants[v].name,identity->variants[v].name);
            }
        }
        if (status!=XR_XIR_OK) return status;
    }
    XrXirTypeMatchScratch scratch={pool->remaining->resources,NULL};XrXirStatus status=XR_XIR_OK;
    for (uint32_t n=0;n<actual->count && status==XR_XIR_OK;++n) {
        if (!xir_compile_work(pool->remaining,1)) { status=XR_XIR_BUDGET;break; }
        const XrXirTypeNode *node=&actual->nodes[n];
        if (node->kind!=XR_XIR_TYPE_NOMINAL || node->parameter_span) continue;
        if (node->nominal.declaration>=count) { status=XR_XIR_BAD_TYPE;break; }
        const XrXirNominalDeclaration *declaration=&source->declarations[mapping[node->nominal.declaration]];
        if (node->nominal.argument_count!=declaration->parameter_count) { status=XR_XIR_BAD_TYPE;break; }
        /* Builder views can precede layout materialization; full instance
         * admission separately requires every exact payload field. */
        if (!node->nominal.field_count && !node->nominal.fields) continue;
        if (node->nominal.field_count!=declaration->field_count ||
            (!!node->nominal.fields!=!!node->nominal.field_count)) { status=XR_XIR_BAD_TYPE;break; }
        for (uint32_t f=0;f<declaration->field_count && status==XR_XIR_OK;++f)
            status=xr_xir_compile_type_substitution_matches_between_scratch(pool->remaining,
                &pool->types,actual,node->nominal.arguments,node->nominal.argument_count,
                declaration->fields[f].type,node->nominal.fields[f],&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);return status;
}
