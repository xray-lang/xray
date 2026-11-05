/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_loans.inc.c - Compiler activation loans on authentic private Cells
 *
 * KEY CONCEPT:
 *   Actual producer roles admit loans; only live parent authority can reborrow.
 *   Every prepared prefix rolls back and every pop restores its exact parent.
 */
static XrXirCtfeStatus ctfe_mutable_owner(ScalarRun *run,
    const XrXirValue *value,XrXirValue *output) {
    if (xr_xir_type_is_cell(run->module->types,(XrXirType)value->type)) {
        const XrXirValue *root=xr_xir_cell_canonical_root(value);if (!root) return XR_XIR_CTFE_BAD_ARTIFACT;
        *output=*root;
    } else *output=*value;
    return xr_xir_type_is_cell(run->module->types,(XrXirType)output->type) ||
        xr_xir_type_is_class(run->module->types,(XrXirType)output->type)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BAD_ARTIFACT;
}
static XrXirCtfeStatus ctfe_mutation_admit(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *owner,const XrXirValue *incoming) {
    XrXirValue key={0};XrXirCtfeStatus status=ctfe_mutable_owner(run,owner,&key);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValue old_cell=state->forbidden_cell,old_class=state->forbidden_class;
    if (xr_xir_type_is_cell(run->module->types,(XrXirType)key.type)) state->forbidden_cell=key;
    else state->forbidden_class=key;
    status=ctfe_value_graph_admit(run,state,incoming);
    state->forbidden_cell=old_cell;state->forbidden_class=old_class;
    return status;
}

#include "xxir_ctfe_projection_bounds.inc.c"
static void ctfe_call_frame_free(CtfeCallFrame *frame) {
    xr_xir_value_drop(&frame->returned);
    xr_xir_panic_drop(&frame->panic);
    while (frame->loan_count) {
        XrXirCellLoan *loan=&frame->loans[--frame->loan_count].loan;
        if (frame->loans_committed) xr_xir_cell_loan_release(loan);else xr_xir_cell_loan_abort(loan);
    }
    ctfe_owned_frame_clear(&frame->run);xr_compile_resources_free(frame);
}
static XrXirCtfeStatus ctfe_call_loans_prepare(CtfeCallExecution *execution,CtfeCallFrame *frame,uint32_t captures) {
    const XrXirModule *module=&execution->artifact->module;CtfeOwnedState *state=&execution->owned;
    const XrXirFunction *body=&module->functions[frame->function];
    uint32_t owner=module->declarations->functions[frame->function].cleanup_owner;
    if (frame->cleanup!=!!owner || (frame->cleanup && (!frame->parent || owner!=frame->parent->function+1 ||
        !frame->parent->state.leaving || !frame->parent->state.cleanup_waiting))) return XR_XIR_CTFE_BAD_ARTIFACT;
    for (uint32_t p=0;p<body->parameter_count;++p) {
        if (!xr_xir_type_is_cell(module->types,body->parameters[p])) continue;
        XrXirCtfeStatus paid=ctfe_value_type_admit(&frame->run,state,body->parameters[p]);if (paid!=XR_XIR_CTFE_OK) return paid;
        XrXirValue value=ctfe_nominal_operand(&frame->run,p);
        XrXirCellProofRole role=xr_xir_cell_provenance_role(state->cells,frame->function,p);
        if (!xr_xir_cell_in_domain(&value,state->domain) || xr_xir_cell_module_storage(&value)) return XR_XIR_CTFE_NOT_SUPPORTED;
        bool projected=xr_xir_cell_is_projection(&value);
        if (role==XR_XIR_CELL_PROOF_OWNED_CAPTURE) {
            if (p>=captures || projected || !xr_xir_cell_unborrowed(&value)) return XR_XIR_CTFE_NOT_SUPPORTED;
            continue;
        }
        if (p<captures || (frame->cleanup?role!=XR_XIR_CELL_PROOF_LEXICAL_CLEANUP:
            role!=XR_XIR_CELL_PROOF_SCOPED_REF)) return XR_XIR_CTFE_NOT_SUPPORTED;
        for (uint32_t prior=0;prior<p;++prior) {
            if (!xr_xir_type_is_cell(module->types,body->parameters[prior])) continue;
            paid=ctfe_value_type_admit(&frame->run,state,body->parameters[p]);if (paid!=XR_XIR_CTFE_OK) return paid;
            paid=ctfe_value_type_admit(&frame->run,state,body->parameters[prior]);if (paid!=XR_XIR_CTFE_OK) return paid;
            XrXirValue other=ctfe_nominal_operand(&frame->run,prior);
            if (xr_xir_cell_same_owner(&value,&other)) return XR_XIR_CTFE_NOT_SUPPORTED;
        }
        XR_CHECK(frame->loan_count<frame->loan_capacity,"compiler frame reserves every possible Cell loan");
        CtfeCellLoan *record=&frame->loans[frame->loan_count];record->value=value;record->projected=projected;
        if (projected) { paid=ctfe_projection_loan_admit(execution,frame,record);if (paid!=XR_XIR_CTFE_OK) return paid; }
        XrXirValueAdmission admission=ctfe_value_admission(state);XrXirFaultDetail fault={0};
        XrXirValueStatus status=projected?xr_xir_cell_loan_prepare_path(&value,&state->authority,&admission,&fault,&record->loan):
            xr_xir_cell_loan_prepare(&value,&state->authority,&record->loan);
        if (status==XR_XIR_VALUE_BOUNDS) return ctfe_bounds_fault(state,fault);
        if (status!=XR_XIR_VALUE_OK) return ctfe_value_status(status);
        ++frame->loan_count;
    }
    return XR_XIR_CTFE_OK;
}
static void ctfe_call_loans_commit(CtfeCallExecution *execution,CtfeCallFrame *frame) {
    XrXirCellAuthority authority={execution,frame,frame->epoch};
    for (uint32_t p=0;p<frame->loan_count;++p)
        xr_xir_cell_loan_commit(&frame->loans[p].value,&frame->loans[p].loan,&authority);
    frame->loans_committed=true;
}
