/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_alias_budget.inc.c - Monotone receiving reserves across real aliases
 *
 * KEY CONCEPT:
 *   A narrow loan never reads its hidden root. Admitted mutable identity keys
 *   and occurrence bounds account for later writes through another real alias.
 */
static XrXirCtfeStatus ctfe_mutable_owner(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *value,XrXirValue *output) {
    if (!scalar_compile_work(run,state->value_header_work+64)) return XR_XIR_CTFE_BUDGET;
    if (xr_xir_type_is_cell(run->module->types,(XrXirType)value->type)) {
        const XrXirValue *root=xr_xir_cell_canonical_root(value);if (!root) return XR_XIR_CTFE_BAD_ARTIFACT;
        *output=*root;
    } else *output=*value;
    return xr_xir_type_is_cell(run->module->types,(XrXirType)output->type) ||
        xr_xir_type_is_class(run->module->types,(XrXirType)output->type)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BAD_ARTIFACT;
}
static XrXirCtfeStatus ctfe_alias_count(ScalarRun *run,CtfeOwnedState *state,
    CtfeCellLoan *record,const XrXirValue *owner,uint64_t *count) {
    XrXirValue root={0};XrXirCtfeStatus status=ctfe_mutable_owner(run,state,&record->value,&root);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (root.type==owner->type && root.payload==owner->payload) { *count=1;return XR_XIR_CTFE_OK; }
    CtfeIdentitySet *set=&record->budget->identities;size_t position=0;
    status=ctfe_identity_find(run,set,owner,&position);if (status!=XR_XIR_CTFE_OK) return status;
    *count=position==set->count?0:set->entries[position].count;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_alias_extend(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *owner,uint64_t work,const CtfeIdentitySet *incoming) {
    if (work>UINT64_MAX/4) return XR_XIR_CTFE_BUDGET;
    for (CtfeCallFrame *frame=state->activation;frame;frame=frame->parent) {
        if (!scalar_compile_work(run,sizeof(*frame))) return XR_XIR_CTFE_BUDGET;
        for (uint32_t p=0;p<frame->loan_count;++p) {
            if (!scalar_compile_work(run,sizeof(CtfeCellLoan))) return XR_XIR_CTFE_BUDGET;
            CtfeCellLoan *record=&frame->loans[p];
            if (!record->projected || !record->budget || !record->budget->tracking_count) continue;
            uint64_t count=0;XrXirCtfeStatus status=ctfe_alias_count(run,state,record,owner,&count);
            if (status!=XR_XIR_CTFE_OK) return status;
            if (count && work*4>UINT64_MAX/count) return XR_XIR_CTFE_BUDGET;
            uint64_t extra=count*work*4;
            if ((count && !record->variable_receiving) || extra>UINT64_MAX-record->root_work) return XR_XIR_CTFE_BUDGET;
            record->root_work+=extra;
        }
    }
    for (CtfeCallFrame *frame=state->activation;frame;frame=frame->parent) {
        if (!scalar_compile_work(run,sizeof(*frame))) return XR_XIR_CTFE_BUDGET;
        for (uint32_t p=0;p<frame->loan_count;++p) {
            if (!scalar_compile_work(run,sizeof(CtfeCellLoan))) return XR_XIR_CTFE_BUDGET;
            CtfeCellLoan *record=&frame->loans[p];
            if (record->budget!=record || !record->tracking_count) continue;
            uint64_t count=0;XrXirCtfeStatus status=ctfe_alias_count(run,state,record,owner,&count);
            if (status!=XR_XIR_CTFE_OK) return status;
            if (!count) continue;
            for (size_t i=0;i<incoming->count;++i) {
                if (!scalar_compile_work(run,sizeof(CtfeIdentityEntry))) return XR_XIR_CTFE_BUDGET;
                if (incoming->entries[i].count>UINT64_MAX/count) return XR_XIR_CTFE_BUDGET;
                status=ctfe_identity_add(run,state,&record->identities,&incoming->entries[i].value,count*incoming->entries[i].count);
                if (status!=XR_XIR_CTFE_OK) return status;
            }
        }
    }
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_mutation_work(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *owner,const XrXirValue *incoming) {
    if (!state->identity_roots || !*state->identity_roots) return ctfe_admission_work(run,state,incoming);
    XrXirValue key={0};XrXirCtfeStatus status=ctfe_mutable_owner(run,state,owner,&key);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (!scalar_compile_work(run,2*sizeof(XrCompileResourceStats))) return XR_XIR_CTFE_BUDGET;
    XrXirValue old_cell=state->forbidden_cell,old_class=state->forbidden_class;
    if (xr_xir_type_is_cell(run->module->types,(XrXirType)key.type)) state->forbidden_cell=key;else state->forbidden_class=key;
    CtfeIdentitySet identities={0};XrCompileResourceStats before={0},after={0};
    if (xr_compile_resources_stats(run->compile_resources,&before)!=XR_COMPILE_RESOURCE_OK) status=XR_XIR_CTFE_BAD_ARTIFACT;
    /* Borrowed summary keys remain pinned by this synchronous incoming graph.
     * Only root-account entries take additional real identity leases. */
    if (status==XR_XIR_CTFE_OK) status=ctfe_value_graph_work(run,state,incoming,ctfe_identity_visit,&identities);
    state->forbidden_cell=old_cell;state->forbidden_class=old_class;
    if (status==XR_XIR_CTFE_OK && (xr_compile_resources_stats(run->compile_resources,&after)!=XR_COMPILE_RESOURCE_OK || after.work<before.work))
        status=XR_XIR_CTFE_BAD_ARTIFACT;
    if (status==XR_XIR_CTFE_OK) status=ctfe_alias_extend(run,state,&key,after.work-before.work,&identities);
    ctfe_identity_drop(&identities);return status;
}
