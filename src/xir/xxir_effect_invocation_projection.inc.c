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
    const EffectInvocationFinal *equations=certificate->final;
    const EffectInvocationNode *node=&equations->nodes[node_index];
    uint32_t parameters=certificate->body->functions[node->root].parameter_count;
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
/* This value belongs only to the present projection request. Each domain
 * keeps its own atom and ordering; no result is retained between requests. */

/* One complete four-domain request uses the same actual atom/cause and
 * advertisement reader as the direct scalar selector below. Each active
 * atom is proved once, with independent projected/constant ROOT and UNKNOWN
 * orderings. This request is never used for a single-domain output.
 * Its output is private caller scratch and may be partially filled on failure.
 * Callers consume it only after success; public outputs have separate gates. */
typedef struct EffectProjectionCauseView {
    const XrXirRootEffectWitness *witness;
    uint32_t callee;
} EffectProjectionCauseView;

/* This view dies before its certificate. No full witness value is copied. */
static inline EffectProjectionCauseView effect_projection_cause_view(
    const EffectInvocationCertificate *certificate,const EffectInvocationNode *node,
    const EffectInvocationCause *cause) {
    const XrXirRootEffectWitness *witness=&cause->witness;
    uint32_t callee=witness->callee;
    if (callee!=UINT32_MAX) callee=certificate->origins[callee].function;
    else if (!witness->distance && node->body!=node->root &&
        (witness->cause==XR_XIR_ROOT_CAUSE_PARAMETER ||
         witness->cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER))
        callee=certificate->origins[node->root].function;
    return (EffectProjectionCauseView){witness,callee};
}

/* This is the original ordering, with the public callee mapping read once. */
static inline bool effect_projection_cause_before(const EffectProjectionCauseView *a,
    const EffectProjectionCauseView *b) {
    if (a->witness->distance!=b->witness->distance)
        return a->witness->distance<b->witness->distance;
    if (a->witness->instruction!=b->witness->instruction)
        return a->witness->instruction<b->witness->instruction;
    if (a->callee!=b->callee) return a->callee<b->callee;
    if (a->witness->cause!=b->witness->cause)
        return a->witness->cause<b->witness->cause;
    return a->witness->slot<b->witness->slot;
}

static XrXirStatus effect_invocation_projection_domains(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,uint32_t first,uint32_t width,
    uint32_t *atoms) {
    if (!xir_compile_context_valid(work) || !certificate || !atoms || first || width!=4 ||
        !effect_body_material_shape(certificate->body) || !certificate->final ||
        !certificate->origins || certificate->resources!=work->resources ||
        certificate->body->resources!=work->resources ||
        function>=effect_body_count(certificate->body)) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *equations=certificate->final;
    if (!equations->roots || !equations->nodes) return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=equations->roots[function];
    if (selected>=equations->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&equations->nodes[selected];
    if (node->body!=function || node->root>=effect_body_count(certificate->body))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t parameters=certificate->body->functions[node->root].parameter_count;
    if (!xir_compile_work(work,9)) return XR_XIR_BUDGET;
    if (!effect_invocation_node_shape(node,parameters,
        certificate->body->functions[function].instruction_count)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,4*sizeof(*atoms))) return XR_XIR_BUDGET;
    for (uint32_t domain=0;domain<4;++domain) atoms[domain]=UINT32_MAX;
    for (uint32_t atom=0;atom<node->cause_count;++atom) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (!effect_invocation_atom(node,parameters,atom,false)) continue;
        uint32_t mask=0;
        XrXirStatus status=effect_invocation_projection_atom(work,certificate,selected,atom,&mask);
        if (status!=XR_XIR_OK) return status;
        const EffectInvocationCause *cause=&node->causes[atom];
        if (cause->witness.callee!=UINT32_MAX &&
            cause->witness.callee>=effect_body_count(certificate->body)) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(work,2+sizeof(EffectProjectionCauseView))) return XR_XIR_BUDGET;
        EffectProjectionCauseView candidate=effect_projection_cause_view(certificate,node,cause);
        for (uint32_t domain=0;domain<4;++domain) {
            if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
            uint32_t bit=(domain&1)?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
            if (((domain&2) && atom>=2) || !(mask&bit)) continue;
            if (atoms[domain]!=UINT32_MAX) {
                if (atoms[domain]>=node->cause_count) return XR_XIR_BAD_STRUCTURE;
                if (!xir_compile_work(work,2+sizeof(EffectProjectionCauseView)+5)) return XR_XIR_BUDGET;
                EffectProjectionCauseView best=effect_projection_cause_view(certificate,node,
                    &node->causes[atoms[domain]]);
                if (!effect_projection_cause_before(&candidate,&best)) continue;
            }
            if (!xir_compile_work(work,sizeof(*atoms))) return XR_XIR_BUDGET;
            atoms[domain]=atom;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_projection_select(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,uint32_t domain,
    uint32_t *atom_output,XrXirRootEffectWitness *witness_output) {
    if (!certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || !certificate->final || !certificate->origins || !atom_output ||
        !witness_output || domain>=4 || function>=effect_body_count(certificate->body))
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *equations=certificate->final;
    uint32_t n=equations->roots[function];
    if (n>=equations->count || equations->nodes[n].body!=function) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&equations->nodes[n];
    if (node->root>=effect_body_count(certificate->body) || node->body>=effect_body_count(certificate->body))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t parameters=certificate->body->functions[node->root].parameter_count;
    if (!xir_compile_work(work,9)) return XR_XIR_BUDGET;
    if (!effect_invocation_node_shape(node,parameters,
        certificate->body->functions[node->body].instruction_count)) return XR_XIR_BAD_STRUCTURE;
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
        if (cause->witness.callee!=UINT32_MAX && cause->witness.callee>=effect_body_count(certificate->body))
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
static XrXirStatus effect_invocation_selected_witness(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,uint32_t atom,
    XrXirRootEffectWitness *output) {
    if (!xir_compile_context_valid(work) || !certificate || !output ||
        !effect_body_material_shape(certificate->body) || !certificate->final ||
        !certificate->origins || certificate->resources!=work->resources ||
        certificate->body->resources!=work->resources ||
        function>=effect_body_count(certificate->body)) return XR_XIR_BAD_STRUCTURE;
    if (!certificate->final->roots || !certificate->final->nodes)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=certificate->final->roots[function];
    if (selected>=certificate->final->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&certificate->final->nodes[selected];
    if (node->body!=function || node->root>=effect_body_count(certificate->body) ||
        (atom!=UINT32_MAX && atom>=node->cause_count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t parameters=certificate->body->functions[node->root].parameter_count;
    if (!xir_compile_work(work,9)) return XR_XIR_BUDGET;
    if (!effect_invocation_node_shape(node,parameters,
        certificate->body->functions[function].instruction_count) ||
        (atom!=UINT32_MAX && !effect_invocation_atom(node,parameters,atom,false)))
        return XR_XIR_BAD_STRUCTURE;
    if (atom!=UINT32_MAX && node->causes[atom].witness.callee!=UINT32_MAX &&
        node->causes[atom].witness.callee>=effect_body_count(certificate->body))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    if (atom==UINT32_MAX) *output=(XrXirRootEffectWitness){0};
    else *output=effect_invocation_public_witness(certificate,node,&node->causes[atom]);
    return XR_XIR_OK;
}

/* These final values belong to the one result owner. A caller subsequently
 * proves each domain against actual causes; these fields alone grant nothing. */
static XrXirStatus effect_invocation_certificate_facts(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,XrXirRootEffects *output) {
    if (!xir_compile_context_valid(work) || !certificate || !output ||
        !effect_body_material_shape(certificate->body) || certificate->resources!=work->resources ||
        certificate->body->resources!=work->resources || !certificate->projection_atoms ||
        !certificate->result || certificate->result->resources!=work->resources ||
        certificate->result->count!=effect_body_count(certificate->body) || !certificate->result->facts ||
        function>=certificate->result->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=certificate->result->facts[function];return XR_XIR_OK;
}

/* The caller provides a freshly proved domain selection. Compare the unique
 * receiving record to its true transformed cause, never to another mirror. */
static XrXirStatus effect_invocation_projection_result_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,uint32_t domain,
    uint32_t atom,const XrXirRootEffectWitness *witness) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->result || !witness ||
        !effect_body_material_shape(certificate->body) || certificate->resources!=work->resources ||
        certificate->body->resources!=work->resources || !certificate->projection_atoms || domain>=4)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationResult *result=certificate->result;
    if (result->resources!=work->resources || result->count!=effect_body_count(certificate->body) ||
        result->capacity!=result->count || function>=result->count || !result->facts ||
        !result->witnesses || !result->functions) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3+sizeof(EffectInvocationProjectedCause))) return XR_XIR_BUDGET;
    size_t at=(size_t)domain*result->count+function;
    uint32_t bit=(domain&1)?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
    bool fact=(domain&2)?!!(result->functions[function].formula.constant_mask&bit):
        (domain&1)?result->facts[function].unresolved:result->facts[function].requires_root;
    if (certificate->projection_atoms[at]!=atom || fact!=(atom!=UINT32_MAX)) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationProjectedCause selected={atom,witness};
    XrXirRootEffectWitness expected;bool symbolic=false;
    XrXirStatus status=effect_invocation_public_transform(work,certificate,function,&selected,&expected,&symbolic);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
    return effect_invocation_witness_equal(&expected,&result->witnesses[at])?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_invocation_certificate_project(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate,EffectInvocationResult *result,
    const EffectInvocationProjectionSink *sink) {
    if (!xir_compile_context_valid(work) || !certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources ||
        certificate->terms.remaining!=work || certificate->projection_atoms || !result || certificate->result!=result ||
        result->resources!=work->resources || result->facts || result->witnesses)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=effect_body_count(certificate->body);uint64_t records=(uint64_t)count*4;
    if (!effect_invocation_projection_sink_matches(work,sink,count) || !count ||
        count!=result->count || result->capacity!=count || !result->functions ||
        records>SIZE_MAX/sizeof(*result->witnesses)) return XR_XIR_BAD_STRUCTURE;
    certificate->projection_atoms=effect_terms_reserve(&certificate->terms,records,sizeof(*certificate->projection_atoms));
    if (!certificate->projection_atoms) return certificate->terms.status;
    XrXirStatus status=XR_XIR_OK;
    result->facts=xir_compile_alloc(work,(size_t)count*sizeof(*result->facts),&status);
    if (!result->facts) return status==XR_XIR_OK?XR_XIR_OUT_OF_MEMORY:status;
    result->witnesses=xir_compile_alloc(work,(size_t)records*sizeof(*result->witnesses),&status);
    if (!result->witnesses) return status==XR_XIR_OK?XR_XIR_OUT_OF_MEMORY:status;
    for (uint32_t f=0;f<count;++f) {
        uint32_t selected[4];
        status=effect_invocation_projection_domains(work,certificate,f,0,4,selected);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t domain=0;domain<4;++domain) {
            size_t at=(size_t)domain*count+f;
            if (!xir_compile_work(work,sizeof(*certificate->projection_atoms))) return XR_XIR_BUDGET;
            certificate->projection_atoms[at]=selected[domain];
            uint32_t bit=(domain&1)?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
            if (domain>=2 && (selected[domain]!=UINT32_MAX)!=
                !!(result->functions[f].formula.constant_mask&bit)) return XR_XIR_BAD_STRUCTURE;
        }
        if (!xir_compile_work(work,sizeof(*result->facts))) return XR_XIR_BUDGET;
        result->facts[f]=(XrXirRootEffects){selected[0]!=UINT32_MAX,selected[1]!=UINT32_MAX};
        status=sink?sink->write(work,certificate,f,selected,result,sink->owner):
            effect_invocation_publication_write(work,certificate,f,selected,result,&result);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
