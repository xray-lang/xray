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
static XrXirCtfeStatus ctfe_projection_record(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *value,CtfeCellLoan **output) {
    CtfeCallFrame *frame=state->activation;if (!frame) return XR_XIR_CTFE_NOT_SUPPORTED;
    for (uint32_t p=0;p<frame->loan_count;++p) {
        if (!scalar_compile_work(run,64)) return XR_XIR_CTFE_BUDGET;
        CtfeCellLoan *record=&frame->loans[p];
        if (!record->projected || record->value.type!=value->type || record->value.payload!=value->payload) continue;
        *output=record;return XR_XIR_CTFE_OK;
    }
    return XR_XIR_CTFE_NOT_SUPPORTED;
}
static XrXirCtfeStatus ctfe_projection_access_work(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *value,bool mutate) {
    CtfeCellLoan *record=NULL;XrXirCtfeStatus status=ctfe_projection_record(run,state,value,&record);
    if (status!=XR_XIR_CTFE_OK) return status;
    status=ctfe_projection_receive_work(run,record);if (status!=XR_XIR_CTFE_OK) return status;
    if (!mutate) return XR_XIR_CTFE_OK;
    if (!scalar_compile_work(run,sizeof(XrXirDomainStats))) return XR_XIR_CTFE_BUDGET;
    XrXirDomainStats stats=xr_xir_domain_stats(state->domain);
    /* An acyclic path cannot clone the same backing twice. Live Domain
     * bytes bound all ancestor copies; shape work also covers retention. */
    if (!stats.live_bytes || record->root_shape>UINT64_MAX/32 ||
        stats.live_bytes>UINT64_MAX/(record->root_shape*32)) return XR_XIR_CTFE_BUDGET;
    return scalar_compile_work(run,stats.live_bytes*record->root_shape*32)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
}
static XrXirCtfeStatus ctfe_projection_extend_work(ScalarRun *run,CtfeOwnedState *state,
    const CtfeCellLoan *record,uint64_t extra) {
    for (CtfeCallFrame *frame=state->activation;frame;frame=frame->parent) {
        if (!scalar_compile_work(run,sizeof(*frame))) return XR_XIR_CTFE_BUDGET;
        for (uint32_t p=0;p<frame->loan_count;++p) {
            if (!scalar_compile_work(run,sizeof(CtfeCellLoan)+state->value_header_work*2)) return XR_XIR_CTFE_BUDGET;
            CtfeCellLoan *other=&frame->loans[p];
            if (!other->projected || !xr_xir_cell_same_owner(&record->value,&other->value)) continue;
            if (!other->variable_receiving || other->root_shape!=record->root_shape) return XR_XIR_CTFE_BAD_ARTIFACT;
            if (extra>UINT64_MAX-other->root_work) return XR_XIR_CTFE_BUDGET;
            other->root_work+=extra;
        }
    }
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_projection_incoming_work(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *cell,const XrXirValue *value) {
    CtfeCellLoan *record=NULL;XrXirCtfeStatus status=ctfe_projection_record(run,state,cell,&record);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (!record->variable_receiving) return ctfe_admission_work(run,state,value);
    if (!scalar_compile_work(run,2*sizeof(XrCompileResourceStats))) return XR_XIR_CTFE_BUDGET;
    XrCompileResourceStats before={0},after={0};
    if (xr_compile_resources_stats(state->context->resources,&before)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_CTFE_BAD_ARTIFACT;
    status=ctfe_admission_work(run,state,value);if (status!=XR_XIR_CTFE_OK) return status;
    if (xr_compile_resources_stats(state->context->resources,&after)!=XR_COMPILE_RESOURCE_OK || after.work<before.work)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    uint64_t work=after.work-before.work;if (work>UINT64_MAX/4) return XR_XIR_CTFE_BUDGET;
    /* A new static composite Array element adds shape*32 receiving work;
     * its own admission pays shape*16. A replacement pays its complete
     * variable subtree. Four times that span bounds either growth, including
     * enclosing fixed metadata. Primitive Array receiving has no length span.
     * This number only bounds later charges; it grants neither work nor access. */
    if (work) { status=ctfe_projection_extend_work(run,state,record,work*4);if (status!=XR_XIR_CTFE_OK) return status; }
    /* Pay the enlarged root before the actual Value write. Failed writes may
     * leave a conservative overestimate; cumulative charges never decrease. */
    return ctfe_projection_receive_work(run,record);
}
static XrXirCtfeStatus ctfe_projection_loan_bound(CtfeCallExecution *execution,CtfeCallFrame *frame,CtfeCellLoan *record) {
    CtfeOwnedState *state=&execution->owned;ScalarRun *run=&frame->run;CtfeCallFrame *parent=state->activation;
    if (parent) for (uint32_t p=0;p<parent->loan_count;++p) {
        if (!scalar_compile_work(run,64+state->value_header_work*2)) return XR_XIR_CTFE_BUDGET;
        const CtfeCellLoan *outer=&parent->loans[p];
        if (!outer->projected || !xr_xir_cell_same_owner(&record->value,&outer->value)) continue;
        record->root_work=outer->root_work;record->root_shape=outer->root_shape;record->variable_receiving=outer->variable_receiving;
        return ctfe_projection_receive_work(run,record);
    }
    if (!scalar_compile_work(run,128+state->value_header_work)) return XR_XIR_CTFE_BUDGET;
    const XrXirValue *root=xr_xir_cell_canonical_root(&record->value);if (!root) return XR_XIR_CTFE_BAD_ARTIFACT;
    bool identity=xr_xir_type_is_class(run->module->types,(XrXirType)root->type);
    XrXirType type=identity?(XrXirType)root->type:xr_xir_cell_element(run->module->types,(XrXirType)root->type);
    if (!identity && !xr_xir_type_is_array(run->module->types,type) && !xr_xir_type_is_struct(run->module->types,type) && !xr_xir_type_is_record(run->module->types,type)) return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirCtfeStatus status=ctfe_validation_work(run,state,type);if (status!=XR_XIR_CTFE_OK) return status;
    if (identity) {
        XrXirType leaf=xr_xir_cell_element(run->module->types,(XrXirType)record->value.type);
        status=ctfe_class_projection_type(run,state,root,leaf,&record->variable_receiving);if (status!=XR_XIR_CTFE_OK) return status;
    }
    XrXirValue value={0};status=ctfe_value_status(identity?xr_xir_value_copy(root,&value):xr_xir_cell_authorized_read(root,&state->authority,&value));
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
    /* Proper descendant loans on data Cells cannot grow the root Array span.
     * Dynamic data leaves on Class roots extend all live parent reserves
     * before writes. No root Value or interior address is retained here. */
    return ctfe_projection_receive_work(run,record);
}
