/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_final_storage.inc.c - Live spans before Final traversal
 */
#define FINAL_STORAGE(pointer,count,type) do { \
    XrXirStatus range_status=effect_program_range(work,pointer,count,sizeof(type),_Alignof(type)); \
    if (range_status!=XR_XIR_OK) return range_status; \
} while (0)

static XrXirStatus effect_program_physical_storage(const XrXirCompileContext *work,
    const EffectPhysicalDomain *physical,uint32_t count) {
    FINAL_STORAGE(physical,1,EffectPhysicalDomain);
    if (physical->resources!=work->resources || physical->count!=count) return XR_XIR_BAD_STRUCTURE;
    FINAL_STORAGE(physical->functions,count,EffectPhysicalFunction);
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        const EffectPhysicalFunction *function=&physical->functions[f];
        if (function->count>function->capacity) return XR_XIR_BAD_STRUCTURE;
        FINAL_STORAGE(function->records,function->capacity,EffectPhysicalRecord);
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_program_bounds_storage(const XrXirCompileContext *work,
    const EffectInvocationDeclaredBounds *bounds,uint32_t count) {
    FINAL_STORAGE(bounds,1,EffectInvocationDeclaredBounds);
    if (bounds->resources!=work->resources || bounds->count!=count || bounds->count>bounds->capacity)
        return XR_XIR_BAD_STRUCTURE;
    FINAL_STORAGE(bounds->functions,bounds->capacity,EffectInvocationFunctionBounds);
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        const EffectInvocationFunctionBounds *function=&bounds->functions[f];
        if (function->count>function->values) return XR_XIR_BAD_STRUCTURE;
        FINAL_STORAGE(function->bounds,function->count,EffectInvocationValueBound);
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_program_outcomes_storage(const XrXirCompileContext *work,
    const EffectFinalOutcomes *outcomes,uint32_t count) {
    FINAL_STORAGE(outcomes,1,EffectFinalOutcomes);
    if (outcomes->resources!=work->resources || outcomes->count!=count ||
        outcomes->atom_count>outcomes->atom_capacity || outcomes->contexts)
        return XR_XIR_BAD_STRUCTURE;
    FINAL_STORAGE(outcomes->error_proof,1,EffectErrorFinite);
    FINAL_STORAGE(outcomes->control_proof,1,EffectControlProof);
    XrCompileResourceStatus error_owner=xr_compile_resources_allocation(work->resources,
        outcomes->error_proof,sizeof(*outcomes->error_proof));
    XrCompileResourceStatus control_owner=error_owner==XR_COMPILE_RESOURCE_OK?
        xr_compile_resources_allocation(work->resources,outcomes->control_proof,
            sizeof(*outcomes->control_proof)):error_owner;
    if (control_owner!=XR_COMPILE_RESOURCE_OK)
        return control_owner==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    FINAL_STORAGE(outcomes->functions,count,XrXirFunctionEffects);
    FINAL_STORAGE(outcomes->witnesses,count,XrXirEffectWitness);
    FINAL_STORAGE(outcomes->atoms,outcomes->atom_capacity,EffectErrorAtom);
    uint64_t words=(uint64_t)outcomes->error_rows*outcomes->words;
    if (words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    FINAL_STORAGE(outcomes->errors,(size_t)words,uint64_t);
    FINAL_STORAGE(outcomes->task_errors,count,XrXirStatus);
    FINAL_STORAGE(outcomes->go_safe,count,XrXirStatus);
    FINAL_STORAGE(outcomes->task_creation,count,XrXirEffect);
    FINAL_STORAGE(outcomes->root,count,XrXirRootEffects);
    FINAL_STORAGE(outcomes->root_witnesses,count,XrXirRootEffectWitness);
    FINAL_STORAGE(outcomes->unresolved_witnesses,count,XrXirRootEffectWitness);
    FINAL_STORAGE(outcomes->contracts,count,XrXirFunctionEffectContract);
    FINAL_STORAGE(outcomes->entry_roots,count,XirEffectEntryRootView);
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(work,6)) return XR_XIR_BUDGET;
        const XrXirFunctionEffectContract *contract=&outcomes->contracts[f];
        FINAL_STORAGE(contract->parameters,contract->parameter_count,XrXirEffectParameter);
        FINAL_STORAGE(contract->formula.terms,contract->formula.term_count,XrXirRootTerm);
        FINAL_STORAGE(contract->values,contract->value_count,XrXirRootValueIdentity);
        FINAL_STORAGE(contract->bindings,contract->binding_count,XrXirEffectCallBinding);
        FINAL_STORAGE(outcomes->entry_roots[f].parameters,outcomes->entry_roots[f].parameter_count,uint32_t);
    }
    return XR_XIR_OK;
}

/* The ledger proves readable current storage only. Every semantic equation,
 * physical role, cause, bound and least result remains an independent duty. */
static XrXirStatus effect_program_final_storage(const XrXirCompileContext *work,
    const XrXirEffects *effects) {
    const EffectBodyMaterial *body=NULL;
    XrXirStatus status=effect_program_input_storage(work,effects,&body);
    if (status!=XR_XIR_OK) return status;
    uint32_t count=effects->count;
    const EffectInvocationCertificate *certificate=effects->invocations;
    FINAL_STORAGE(certificate->final,1,EffectInvocationFinal);
    const EffectInvocationFinal *final=certificate->final;
    if (final->material!=body || final->physical!=body->physical || final->cells!=body->cells ||
        effects->outcomes_owner || effects->root_input ||
        final->count>final->capacity || final->edge_count>final->edge_capacity ||
        final->deferred_count>final->deferred_capacity) return XR_XIR_BAD_STRUCTURE;
    const EffectRoleProof *roles=&final->role_proof;
    if (roles->functions>count) return XR_XIR_BAD_STRUCTURE;
    FINAL_STORAGE(roles->rows,roles->functions,EffectRoleFunction);
    FINAL_STORAGE(roles->facts,roles->total,EffectRoleFact);
    if (final->capture.count>final->capture.capacity) return XR_XIR_BAD_STRUCTURE;
    FINAL_STORAGE(final->capture.facts,final->capture.capacity,EffectCaptureFact);
    status=effect_program_outcomes_storage(work,final->outcomes,count);
    if (status!=XR_XIR_OK) return status;
    status=effect_program_physical_storage(work,final->physical,count);
    if (status!=XR_XIR_OK) return status;
    status=effect_program_bounds_storage(work,certificate->declared,count);
    if (status!=XR_XIR_OK) return status;
    status=xir_cell_provenance_storage(work,final->cells,count);
    if (status!=XR_XIR_OK) return status;
    FINAL_STORAGE(certificate->origins,count,XrXirOrigin);
    FINAL_STORAGE(certificate->owners,count,uint32_t);
    FINAL_STORAGE(final->roots,count,uint32_t);
    FINAL_STORAGE(final->root_spaces,count,uint32_t);
    FINAL_STORAGE(final->local_roots,count,XrXirRootEffects);
    FINAL_STORAGE(final->local_witnesses,(size_t)count*2,XrXirRootEffectWitness);
    FINAL_STORAGE(body->control,count,XrXirFunctionEffects);
    FINAL_STORAGE(effects->formula_terminals,(size_t)count*4,XrXirRootEffectWitness);
    FINAL_STORAGE(effects->constant_witnesses,(size_t)count*2,XrXirRootEffectWitness);
    const EffectFinalFunctions *summary=&final->summary;
    if (summary->resources!=work->resources || summary->count!=count || summary->capacity!=count)
        return XR_XIR_BAD_STRUCTURE;
    FINAL_STORAGE(summary->functions,count,EffectFinalFunction);
    FINAL_STORAGE(summary->witnesses,(size_t)count*4,XrXirRootEffectWitness);
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
        FINAL_STORAGE(certificate->origins[f].arguments,certificate->origins[f].argument_count,XrXirType);
        FINAL_STORAGE(certificate->origins[f].effect_arguments,certificate->origins[f].effect_argument_count,XrXirEffectArgument);
        FINAL_STORAGE(summary->functions[f].storage,summary->functions[f].capacity,XrXirRootTerm);
        FINAL_STORAGE(summary->functions[f].formula.terms,summary->functions[f].formula.term_count,XrXirRootTerm);
    }
    FINAL_STORAGE(final->nodes,final->capacity,EffectInvocationNode);
    FINAL_STORAGE(final->edges,final->edge_capacity,EffectInvocationEdge);
    FINAL_STORAGE(final->sites,final->site_count,EffectInvocationSite);
    FINAL_STORAGE(final->deferred_calls,final->deferred_capacity,EffectInvocationDeferred);
    for (uint32_t n=0;n<final->count;++n) {
        if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&final->nodes[n];
        if (node->root>=count || node->body>=count || node->producer_count>node->producer_capacity)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t parameters=body->functions[node->root].parameter_count;
        uint32_t instructions=body->functions[node->body].instruction_count;
        if (node->parameter_count!=body->functions[node->body].parameter_count ||
            node->instruction_count!=instructions || node->cause_count!=(uint64_t)parameters*2+3)
            return XR_XIR_BAD_STRUCTURE;
        EffectInvocationBasis basis;
        status=effect_invocation_basis_parameters_read(work,final,parameters,&basis);
        if (status!=XR_XIR_OK) return status;
        uint64_t input_words=(uint64_t)node->parameter_count*basis.row_words;
        uint64_t output_words=(((uint64_t)parameters+63)/64)*2+((uint64_t)instructions+63)/64;
        if (input_words>SIZE_MAX/sizeof(uint64_t) || output_words>SIZE_MAX/sizeof(uint64_t) ||
            (uint64_t)node->cause_count*2>SIZE_MAX/sizeof(EffectInvocationCause)) return XR_XIR_BUDGET;
        FINAL_STORAGE(node->causes,(size_t)node->cause_count*2,EffectInvocationCause);
        FINAL_STORAGE(node->input,(size_t)input_words,uint64_t);
        FINAL_STORAGE(node->cells,(size_t)output_words,uint64_t);
        FINAL_STORAGE(node->local_cells,(size_t)output_words,uint64_t);
        FINAL_STORAGE(node->parameter_coverage,node->parameter_count,uint64_t);
        FINAL_STORAGE(node->producers,node->producer_capacity,EffectInvocationProducer);
        FINAL_STORAGE(node->classes,node->class_count,EffectInvocationClassFact);
    }
    for (uint32_t d=0;d<final->deferred_count;++d) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        FINAL_STORAGE(final->deferred_calls[d].arguments,final->deferred_calls[d].count,uint64_t);
    }
    return XR_XIR_OK;
}
#undef FINAL_STORAGE
