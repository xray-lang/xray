/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_projection.inc.c - Owned advertisement and constant projections
 *
 * KEY CONCEPT:
 *   Four projections select rooted equation atoms. Actual edge identities stay
 *   in the same certificate instead of becoming a declaration-only call chain.
 */
static XrXirRootEffectWitness effect_invocation_public_witness(
    const EffectInvocationCertificate *certificate,const EffectInvocationNode *node,
    const EffectInvocationCause *cause) {
    XrXirRootEffectWitness witness=cause->witness;
    if (witness.callee!=UINT32_MAX)
        witness.callee=certificate->origins[witness.callee].function;
    else if (!witness.distance && node->body!=node->root &&
        (witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER || witness.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER))
        witness.callee=certificate->origins[node->root].function;
    return witness;
}

static XrXirStatus effect_invocation_projection_atom(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t node_index,uint32_t atom,uint32_t *output) {
    const EffectInvocationOwner *equations=certificate->equations;
    const EffectInvocationNode *node=&equations->nodes[node_index];
    uint32_t parameters=certificate->bodies.functions[node->root].parameter_count;
    if (atom>=node->cause_count || !effect_invocation_atom(node,parameters,atom,false))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_certificate_cause(work,certificate,node_index,atom);
    if (status!=XR_XIR_OK) return status;
    uint32_t mask=atom<2 ? atom ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : XR_XIR_CALLABLE_ROOT_REQUIRED :
        XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    if (atom>=2 && atom-2<parameters) {
        uint32_t p=atom-2;
        if (!certificate->parameter_kinds || !certificate->parameter_kinds[node->root] ||
            certificate->parameter_kinds[node->root][p]!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
            return XR_XIR_BAD_STRUCTURE;
        status=effect_invocation_bounds_mask(work,certificate->declared,node->root,p,&mask);
        if (status!=XR_XIR_OK) return status;
    }
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    *output=mask;return XR_XIR_OK;
}

/* Constant domains consume only true intrinsic atoms. Projected domains also
 * consume the owned original formal advertisements and unresolved dependencies.
 * NONE advertisements never authorize an unrooted or incomplete equation. */
static XrXirStatus effect_invocation_projection_select(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,uint32_t domain,
    uint32_t *atom_output,XrXirRootEffectWitness *witness_output) {
    if (!certificate || !certificate->equations || !certificate->origins || !atom_output ||
        !witness_output || domain>=4 || function>=certificate->bodies.function_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationOwner *equations=certificate->equations;
    uint32_t n=equations->roots[function];
    if (n>=equations->count || equations->nodes[n].body!=function) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&equations->nodes[n];
    uint32_t parameters=certificate->bodies.functions[node->root].parameter_count;
    uint32_t bit=(domain&1)?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
    uint32_t selected=UINT32_MAX;XrXirRootEffectWitness best={0};
    uint32_t count=(domain&2)?2:node->cause_count;
    for (uint32_t atom=0;atom<count;++atom) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (!effect_invocation_atom(node,parameters,atom,false)) continue;
        uint32_t mask=0;
        XrXirStatus status=effect_invocation_projection_atom(work,certificate,n,atom,&mask);
        if (status!=XR_XIR_OK) return status;
        if (!(mask&bit)) continue;
        const EffectInvocationCause *cause=&node->causes[atom];
        if (cause->witness.callee!=UINT32_MAX && cause->witness.callee>=certificate->bodies.function_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirRootEffectWitness candidate=effect_invocation_public_witness(certificate,node,cause);
        if (selected!=UINT32_MAX && (best.distance<candidate.distance ||
            (best.distance==candidate.distance && !effect_root_cause_before(&candidate,&best)))) continue;
        selected=atom;best=candidate;
    }
    if (!xir_compile_work(work,sizeof(best)+1)) return XR_XIR_BUDGET;
    *atom_output=selected;*witness_output=best;return XR_XIR_OK;
}

/* All records remain private to this not-yet-published certificate. No external
 * Effects, contract, carrier or entry permission changes on any failure path. */
static XrXirStatus effect_invocation_certificate_project(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate) {
    if (!xir_compile_context_valid(work) || !certificate || certificate->resources!=work->resources ||
        certificate->terms.remaining!=work || !certificate->solution || certificate->facts ||
        certificate->witnesses || certificate->projection_atoms) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=certificate->bodies.function_count;
    uint64_t records=(uint64_t)count*4;
    if (!count || count!=certificate->solution->count || records>SIZE_MAX/sizeof(*certificate->witnesses))
        return count?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    certificate->facts=effect_terms_alloc(&certificate->terms,count,sizeof(*certificate->facts));
    certificate->witnesses=effect_terms_alloc(&certificate->terms,records,sizeof(*certificate->witnesses));
    certificate->projection_atoms=effect_terms_alloc(&certificate->terms,records,sizeof(*certificate->projection_atoms));
    if (!certificate->facts || !certificate->witnesses || !certificate->projection_atoms)
        return certificate->terms.status;
    for (uint32_t f=0;f<count;++f) {
        uint32_t mask=0;
        for (uint32_t domain=0;domain<4;++domain) {
            size_t at=(size_t)domain*count+f;
            XrXirStatus status=effect_invocation_projection_select(work,certificate,f,domain,
                &certificate->projection_atoms[at],&certificate->witnesses[at]);
            if (status!=XR_XIR_OK) return status;
            bool fact=certificate->projection_atoms[at]!=UINT32_MAX;
            uint32_t bit=(domain&1)?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
            if (domain<2 && fact) mask|=bit;
            if (domain>=2 && fact!=!!(certificate->solution->formulas[f].constant_mask&bit))
                return XR_XIR_BAD_STRUCTURE;
        }
        if (!xir_compile_work(work,sizeof(*certificate->facts))) return XR_XIR_BUDGET;
        certificate->facts[f]=(XrXirRootEffects){!!(mask&XR_XIR_CALLABLE_ROOT_REQUIRED),
            !!(mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)};
    }
    return XR_XIR_OK;
}
