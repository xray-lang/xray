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
 *   actual reborrow authority; every current descriptor is checked by Value.
 */

static XrXirCtfeStatus ctfe_projection_record(CtfeOwnedState *state,
    const XrXirValue *value,CtfeCellLoan **output) {
    CtfeCallFrame *frame=state->activation;if (!frame) return XR_XIR_CTFE_NOT_SUPPORTED;
    for (uint32_t p=0;p<frame->loan_count;++p) {
        CtfeCellLoan *record=&frame->loans[p];
        if (!record->projected || record->value.type!=value->type || record->value.payload!=value->payload) continue;
        *output=record;return XR_XIR_CTFE_OK;
    }
    return XR_XIR_CTFE_NOT_SUPPORTED;
}
static XrXirCtfeStatus ctfe_projection_access_admit(CtfeOwnedState *state,
    const XrXirValue *value,uint32_t suffix) {
    CtfeCellLoan *record=NULL;
    XrXirCtfeStatus status=ctfe_projection_record(state,value,&record);
    if (status!=XR_XIR_CTFE_OK) return status;
    uint64_t span=0;
    return ctfe_projection_path_admit(state,value,suffix,&span);
}

static XrXirCtfeStatus ctfe_projection_incoming_admit(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *cell,const XrXirValue *value) {
    CtfeCellLoan *record=NULL;
    XrXirCtfeStatus status=ctfe_projection_record(state,cell,&record);
    if (status!=XR_XIR_CTFE_OK) return status;
    return ctfe_mutation_admit(run,state,cell,value);
}
static XrXirCtfeStatus ctfe_projection_loan_admit(CtfeCallExecution *execution,CtfeCallFrame *frame,CtfeCellLoan *record) {
    CtfeOwnedState *state=&execution->owned;ScalarRun *run=&frame->run;
    uint64_t span=0;
    XrXirCtfeStatus status=ctfe_projection_path_admit(state,&record->value,0,&span);
    if (status!=XR_XIR_CTFE_OK) return status;
    /* The parent loan already admitted its canonical root. A reborrow cannot
     * inspect the root through a narrower authority; Value checks its path. */
    if (state->activation) for (uint32_t p=0;p<state->activation->loan_count;++p) {
        const CtfeCellLoan *outer=&state->activation->loans[p];
        if (outer->projected && xr_xir_cell_same_owner(&record->value,&outer->value)) return XR_XIR_CTFE_OK;
    }
    const XrXirValue *root=xr_xir_cell_canonical_root(&record->value);
    if (!root) return XR_XIR_CTFE_BAD_ARTIFACT;
    bool identity=xr_xir_type_is_class(run->module->types,(XrXirType)root->type);
    XrXirType type=identity?(XrXirType)root->type:xr_xir_cell_element(run->module->types,(XrXirType)root->type);
    if (!identity && !xr_xir_type_is_array(run->module->types,type) && !xr_xir_type_is_struct(run->module->types,type))
        return XR_XIR_CTFE_NOT_SUPPORTED;
    status=ctfe_value_type_admit(run,state,type);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValue value={0};
    status=ctfe_value_status(identity?xr_xir_value_copy(root,&value):xr_xir_cell_authorized_read(root,&state->authority,&value));
    if (status==XR_XIR_CTFE_OK) status=ctfe_value_graph_admit(run,state,&value);
    xr_xir_value_drop(&value);return status;
}
