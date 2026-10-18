/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_view.inc.c - Borrowed values through real owned interpreter scratch
 *
 * KEY CONCEPT:
 *   The frame owns each transient full Value until publication or rollback.
 */
static XrXirRunStatus vm_view_step(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,uint32_t destination,XrXirAction *action) {
    bool group=xr_xir_op_uses_operand_table(op->op);
    const uint32_t *args=group ? run->function->operands+op->args[0] : op->args;
    XrXirValue root=vm_value_operand(run,args[0]),element={0};
    XrXirValue *scratch=state->arguments;
    scratch[0]=scratch[1]=(XrXirValue){0};
    XrXirCallViewOperation request={.operation=op->op,.type=op->type,.root=&root};
    XrXirValueStatus status=XR_XIR_VALUE_OK;bool ended=op->op==XR_XIR_VIEW_END;
    if (op->op==XR_XIR_VIEW_NEW) {
        status=xr_xir_value_copy(&root,scratch);
        request.root=scratch;
    } else if (ended) {
        scratch[0]=root;
        xr_xir_scalar_store(run->frame,run->layout->offsets[args[0]],0);
        request.root=NULL;request.owned=scratch;
    }
    if (group) {
        request.start=xr_xir_scalar_load(run->frame,run->layout->offsets[args[1]]);
        if (op->op==XR_XIR_VIEW_SET) {element=vm_value_operand(run,args[2]);request.element=&element;}
        else {request.end=xr_xir_scalar_load(run->frame,run->layout->offsets[args[2]]);request.to_end=op->immediate!=0;}
    } else if (op->op==XR_XIR_VIEW_GET)
        request.start=xr_xir_scalar_load(run->frame,run->layout->offsets[args[1]]);
    XrXirFaultDetail fault={0};
    if (status==XR_XIR_VALUE_OK) status=xr_xir_call_view_operation(run->view,&request,scratch+1,&fault);
    if (ended && status!=XR_XIR_VALUE_OK)
        xr_xir_owned_slot_move(run->frame,run->layout->offsets[args[0]],scratch);
    else xr_xir_value_drop(scratch);
    if (status==XR_XIR_VALUE_OK && op->type!=XR_XIR_UNIT) {
        if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,destination,scratch+1);
        else {xr_xir_scalar_store(run->frame,destination,scratch[1].payload);scratch[1]=(XrXirValue){0};}
    }
    xr_xir_value_drop(scratch+1);
    if (status!=XR_XIR_VALUE_OK) *action=xr_xir_call_view_operation_fault(status,fault);
    return XR_XIR_RUN_OK;
}
static XrXirAction vm_view_return(ScalarRun *run,VmState *state,const XrXirInstruction *op) {
    XrXirValue *scratch=state->arguments;*scratch=vm_value_operand(run,op->args[0]);
    uint32_t offset=run->layout->offsets[op->args[0]];
    xr_xir_scalar_store(run->frame,offset,0);
    XrXirAction action={0};XrXirCallStatus status=xr_xir_call_return_move(run->view,scratch,&action);
    if (status!=XR_XIR_CALL_READY) {
        xr_xir_owned_slot_move(run->frame,offset,scratch);
        return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
    }
    return action;
}
