/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_projection_bounds.inc.c - Receiving reserves on real scoped loans
 *
 * KEY CONCEPT:
 *   A narrow loan cannot inspect its whole root. Its immediate parent supplies
 *   a prepaid receiving bound, never additional access authority or Value data.
 */
static XrXirCtfeStatus ctfe_projection_receive_work(ScalarRun *run,const CtfeCellLoan *record) {
    if (!record->root_shape || record->root_shape>=UINT64_MAX/4 ||
        record->root_work>UINT64_MAX/((record->root_shape+1)*4)) return XR_XIR_CTFE_BUDGET;
    return scalar_compile_work(run,record->root_work*(record->root_shape+1)*4)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
}
static XrXirCtfeStatus ctfe_projection_access_work(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *value,bool mutate) {
    CtfeCallFrame *frame=state->activation;if (!frame) return XR_XIR_CTFE_NOT_SUPPORTED;
    for (uint32_t p=0;p<frame->loan_count;++p) {
        if (!scalar_compile_work(run,64)) return XR_XIR_CTFE_BUDGET;
        const CtfeCellLoan *record=&frame->loans[p];
        if (!record->projected || record->value.type!=value->type || record->value.payload!=value->payload) continue;
        XrXirCtfeStatus status=ctfe_projection_receive_work(run,record);if (status!=XR_XIR_CTFE_OK) return status;
        if (!mutate) return XR_XIR_CTFE_OK;
        if (!scalar_compile_work(run,sizeof(XrXirDomainStats))) return XR_XIR_CTFE_BUDGET;
        XrXirDomainStats stats=xr_xir_domain_stats(state->domain);
        /* An acyclic path cannot clone the same backing twice. Live Domain
         * bytes bound all ancestor copies; shape work also covers retention. */
        if (!stats.live_bytes || record->root_shape>UINT64_MAX/32 ||
            stats.live_bytes>UINT64_MAX/(record->root_shape*32)) return XR_XIR_CTFE_BUDGET;
        return scalar_compile_work(run,stats.live_bytes*record->root_shape*32)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
    }
    return XR_XIR_CTFE_NOT_SUPPORTED;
}
static XrXirCtfeStatus ctfe_projection_loan_bound(CtfeCallExecution *execution,CtfeCallFrame *frame,CtfeCellLoan *record) {
    CtfeOwnedState *state=&execution->owned;ScalarRun *run=&frame->run;CtfeCallFrame *parent=state->activation;
    if (parent) for (uint32_t p=0;p<parent->loan_count;++p) {
        if (!scalar_compile_work(run,64+state->value_header_work*2)) return XR_XIR_CTFE_BUDGET;
        const CtfeCellLoan *outer=&parent->loans[p];
        if (!outer->projected || !xr_xir_cell_same_owner(&record->value,&outer->value)) continue;
        record->root_work=outer->root_work;record->root_shape=outer->root_shape;
        return ctfe_projection_receive_work(run,record);
    }
    if (!scalar_compile_work(run,128+state->value_header_work)) return XR_XIR_CTFE_BUDGET;
    const XrXirValue *root=xr_xir_cell_canonical_root(&record->value);if (!root) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirType type=xr_xir_cell_element(run->module->types,(XrXirType)root->type);
    if (!xr_xir_type_is_array(run->module->types,type) && !xr_xir_type_is_struct(run->module->types,type)) return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirCtfeStatus status=ctfe_validation_work(run,state,type);if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValue value={0};status=ctfe_value_status(xr_xir_cell_authorized_read(root,&state->authority,&value));
    if (status!=XR_XIR_CTFE_OK) return status;
    XrCompileResourceStats before={0},after={0};
    if (xr_compile_resources_stats(state->context->resources,&before)!=XR_COMPILE_RESOURCE_OK) status=XR_XIR_CTFE_BAD_ARTIFACT;
    if (status==XR_XIR_CTFE_OK) status=ctfe_admission_work(run,state,&value);
    if (status==XR_XIR_CTFE_OK && xr_compile_resources_stats(state->context->resources,&after)!=XR_COMPILE_RESOURCE_OK) status=XR_XIR_CTFE_BAD_ARTIFACT;
    xr_xir_value_drop(&value);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (after.work<before.work) return XR_XIR_CTFE_BAD_ARTIFACT;
    record->root_work=after.work-before.work;
    record->root_shape=state->tuple_costs[type-XR_XIR_CONSTRUCTED_TYPE_BASE].work;
    /* Admitted dynamic roots have one variable Array receiving span, and its
     * length cannot change through a proper descendant loan. Other admitted
     * roots have static receiving spans. No root Value is retained here. */
    return ctfe_projection_receive_work(run,record);
}
