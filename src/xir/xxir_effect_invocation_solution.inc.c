/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_solution.inc.c - Exact closed equations and staged canonical formulas
 *
 * KEY CONCEPT:
 *   A staged result owns its canonical records. No borrowed producer, pending
 *   equation or merely empty lattice state can become an executable permission.
 */
typedef struct EffectInvocationSolution {
    XrCompileResources *resources;
    uint32_t count,term_count;
    XrXirRootFormula *formulas;
    void *storage;
} EffectInvocationSolution;
_Static_assert(sizeof(XrXirRootFormula)%_Alignof(XrXirRootTerm)==0,
    "Packed canonical term records must remain aligned");

static void effect_invocation_solution_free(EffectInvocationSolution *solution) {
    if (!solution) return;
    xr_compile_resources_free(solution->storage);xr_compile_resources_free(solution);
}

static bool effect_invocation_frontier(XrXirOp op) {
    return op==XR_XIR_CALL || op==XR_XIR_INVOKE || op==XR_XIR_CALL_DEFAULT ||
        op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_CALL_REQUIREMENT ||
        op==XR_XIR_CLEANUP_REGISTER || op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT;
}

/* Compare the exact union of local contributions and executing children.
 * An inflated OR result is rejected, including an unrooted recursive fact.
 * Certificate forests separately prove a genuine local terminal for every bit. */
static XrXirStatus effect_invocation_closed(EffectInvocationOwner *owner) {
    if (!owner || !owner->module || !owner->graph || owner->pending || !owner->count)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t *expected=NULL;size_t capacity=0;
    XrXirStatus status=XR_XIR_OK;uint64_t linked=0;
    for (uint32_t n=0;n<owner->count && status==XR_XIR_OK;++n) {
        const EffectInvocationNode *node=&owner->nodes[n];
        if (!node->expanded || node->queued || node->body>=owner->module->function_count) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        EffectInvocationBasis basis={0};status=effect_invocation_basis(owner,node->root,&basis);
        if (status!=XR_XIR_OK) break;
        uint32_t dependencies=(uint32_t)(((uint64_t)basis.parameters+63)/64);
        uint32_t instructions=owner->module->functions[node->body].instruction_count;
        uint32_t contexts=(uint32_t)(((uint64_t)instructions+63)/64);
        uint64_t words=(uint64_t)dependencies*2+contexts;
        if (words>SIZE_MAX/sizeof(*expected) || (words && (!node->outputs || !node->local_cells))) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (words>capacity) {
            uint64_t *replacement=xir_compile_alloc(owner->work,(size_t)words*sizeof(*expected),&status);
            if (!replacement) break;
            xr_compile_resources_free(expected);expected=replacement;capacity=(size_t)words;
        }
        if (!xir_compile_work(owner->work,words*sizeof(*expected)+3)) { status=XR_XIR_BUDGET;break; }
        if (words) memcpy(expected,node->local_cells,(size_t)words*sizeof(*expected));
        uint32_t mask=node->local_mask;bool deferred=node->local_deferred;
        for (uint32_t e=node->edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
            if (!xir_compile_work(owner->work,6)) { status=XR_XIR_BUDGET;break; }
            if (e>=owner->edge_count) { status=XR_XIR_BAD_STRUCTURE;break; }
            EffectInvocationEdge edge=owner->edges[e];++linked;
            if (linked>owner->edge_count || edge.caller!=n || edge.target>=owner->count ||
                edge.instruction>=instructions || (edge.next!=UINT32_MAX && edge.next>=e) ||
                owner->nodes[edge.target].root!=node->root) { status=XR_XIR_BAD_STRUCTURE;break; }
            if (edge.latent) continue;
            const EffectInvocationNode *child=&owner->nodes[edge.target];
            mask|=child->intrinsic_mask;deferred|=child->deferred;
            for (uint32_t w=0;w<dependencies;++w) {
                if (!xir_compile_work(owner->work,4)) { status=XR_XIR_BUDGET;break; }
                expected[w]|=child->cells[w];expected[dependencies+w]|=child->parameters[w];
            }
            if (status!=XR_XIR_OK) break;
            if (child->deferred) expected[(size_t)dependencies*2+edge.instruction/64]|=
                UINT64_C(1)<<(edge.instruction%64);
        }
        if (status!=XR_XIR_OK) break;
        if (mask!=node->intrinsic_mask || deferred!=node->deferred ||
            (mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED))) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (basis.parameters%64) {
            uint64_t valid=(UINT64_C(1)<<(basis.parameters%64))-1;
            if ((node->cells[dependencies-1]|node->parameters[dependencies-1])&~valid) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
        }
        if (instructions%64 && (node->contexts[contexts-1]&
            ~((UINT64_C(1)<<(instructions%64))-1))) { status=XR_XIR_BAD_STRUCTURE;break; }
        bool any=false;
        for (size_t w=0;w<(size_t)words;++w) {
            if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
            if (expected[w]!=node->outputs[w]) { status=XR_XIR_BAD_STRUCTURE;break; }
        }
        for (uint32_t i=0;i<instructions && status==XR_XIR_OK;++i) {
            if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
            if (!(node->contexts[i/64]&(UINT64_C(1)<<(i%64)))) continue;
            any=true;
            if (!effect_invocation_frontier(owner->module->functions[node->body].instructions[i].op))
                status=XR_XIR_BAD_STRUCTURE;
        }
        if (status==XR_XIR_OK && any!=deferred) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(expected);
    if (status==XR_XIR_OK && linked!=owner->edge_count) status=XR_XIR_BAD_STRUCTURE;
    return status;
}

static XrXirStatus effect_invocation_terms(EffectInvocationOwner *owner,
    uint32_t function,XrXirRootTerm *terms,uint32_t *count) {
    if (!count || function>=owner->module->function_count || owner->roots[function]>=owner->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->nodes[owner->roots[function]];
    const XrXirFunction *body=&owner->module->functions[function];
    if (!owner->root_spaces || node->root!=owner->root_spaces[function] ||
        node->body!=function || node->parameter_count!=body->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    bool shared=node->root!=function;
    if (shared) for (uint32_t p=0;p<body->parameter_count;++p) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(owner->module->types,body->parameters[p]) ||
            xr_xir_type_is_cell(owner->module->types,body->parameters[p])) return XR_XIR_BAD_STRUCTURE;
    }
    if (shared) {
        EffectInvocationBasis basis={0};
        XrXirStatus status=effect_invocation_basis(owner,node->root,&basis);
        if (status!=XR_XIR_OK) return status;
        uint32_t words=(uint32_t)(((uint64_t)basis.parameters+63)/64);
        for (uint32_t w=0;w<words;++w) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            if (node->cells[w] || node->parameters[w]) return XR_XIR_BAD_STRUCTURE;
        }
    }
    uint64_t at=0;
    for (uint32_t kind=XR_XIR_ROOT_TERM_PARAMETER;kind<=XR_XIR_ROOT_TERM_CELL_PARAMETER;++kind) {
        uint32_t items=kind==XR_XIR_ROOT_TERM_CONTEXT_CALL?body->instruction_count:shared?0:body->parameter_count;
        const uint64_t *bits=kind==XR_XIR_ROOT_TERM_PARAMETER?node->parameters:
            kind==XR_XIR_ROOT_TERM_CELL_PARAMETER?node->cells:node->contexts;
        for (uint32_t i=0;i<items;++i) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            if (!(bits[i/64]&(UINT64_C(1)<<(i%64)))) continue;
            if (kind==XR_XIR_ROOT_TERM_PARAMETER &&
                (!xr_xir_callable_signature(owner->module->types,body->parameters[i]) ||
                owner->effects->contracts[function].parameters[i].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE))
                return XR_XIR_BAD_STRUCTURE;
            if (kind==XR_XIR_ROOT_TERM_CELL_PARAMETER && !xr_xir_type_is_cell(owner->module->types,body->parameters[i]))
                return XR_XIR_BAD_STRUCTURE;
            if (kind==XR_XIR_ROOT_TERM_CONTEXT_CALL && !effect_invocation_frontier(body->instructions[i].op))
                return XR_XIR_BAD_STRUCTURE;
            if (at>=UINT32_MAX) return XR_XIR_BUDGET;
            if (terms) {
                if (!xir_compile_work(owner->work,sizeof(*terms))) return XR_XIR_BUDGET;
                terms[at]=(XrXirRootTerm){kind,i};
            }
            ++at;
        }
    }
    *count=(uint32_t)at;return XR_XIR_OK;
}

/* This stage does not write Effects, contracts, carriers or permission tables.
 * Its caller must retain the complete sealed equation/cause certificate before
 * one final atomic publication through the common owner. */
static XrXirStatus effect_invocation_stage(EffectInvocationOwner *owner,
    EffectInvocationSolution **output) {
    if (!output || *output || !owner || !owner->effects || !owner->effects->contracts)
        return XR_XIR_BAD_STRUCTURE;
    /* Formulas depend on closed facts. The sealed owner constructs and
     * verifies the complete cause forest once, before any publication. */
    XrXirStatus status=effect_invocation_closed(owner);
    uint32_t count=owner->module->function_count;
    uint64_t term_count=0;
    for (uint32_t f=0;f<count && status==XR_XIR_OK;++f) {
        uint32_t terms=0;status=effect_invocation_terms(owner,f,NULL,&terms);term_count+=terms;
        if (term_count>UINT32_MAX) status=XR_XIR_BUDGET;
    }
    uint64_t bytes=(uint64_t)count*sizeof(XrXirRootFormula)+term_count*sizeof(XrXirRootTerm);
    if (status==XR_XIR_OK && bytes>SIZE_MAX) status=XR_XIR_BUDGET;
    EffectInvocationSolution *solution=status==XR_XIR_OK ?
        xir_compile_calloc(owner->work,1,sizeof(*solution),&status) : NULL;
    if (status==XR_XIR_OK && !solution) status=XR_XIR_OUT_OF_MEMORY;
    if (status==XR_XIR_OK) {
        solution->resources=owner->work->resources;solution->count=count;solution->term_count=(uint32_t)term_count;
        solution->storage=bytes?xir_compile_calloc(owner->work,1,(size_t)bytes,&status):NULL;
        if (status==XR_XIR_OK && bytes && !solution->storage) status=XR_XIR_OUT_OF_MEMORY;
        solution->formulas=solution->storage;
    }
    XrXirRootTerm *records=solution && solution->storage ? (XrXirRootTerm *)(solution->formulas+count):NULL;
    uint32_t at=0;
    for (uint32_t f=0;f<count && status==XR_XIR_OK;++f) {
        uint32_t terms=0;
        status=effect_invocation_terms(owner,f,records?records+at:NULL,&terms);
        if (status!=XR_XIR_OK) break;
        if (at>term_count || terms>term_count-at || !xir_compile_work(owner->work,sizeof(XrXirRootFormula))) {
            status=at>term_count || terms>term_count-at?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET;break;
        }
        solution->formulas[f]=(XrXirRootFormula){owner->nodes[owner->roots[f]].intrinsic_mask,
            terms,terms?records+at:NULL};at+=terms;
    }
    if (status==XR_XIR_OK && at!=term_count) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { effect_invocation_solution_free(solution);return status; }
    *output=solution;return XR_XIR_OK;
}
