/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_fixed_array.inc.c - Owned nonempty fixed byte execution
 *
 * KEY CONCEPT:
 *   Real operands build one owner and bounds faults preserve observed operands.
 */
static XrXirRunStatus vm_fixed_array_step(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,uint32_t destination,XrXirAction *action) {
    XrXirValueAdmission *admission=xr_xir_call_admission(run->view);
    if(!admission)return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue output={0};XrXirFaultDetail fault={0};XrXirValueStatus status;
    if(op->op==XR_XIR_FIXED_ARRAY_NEW) {
        if(op->args[1]>run->layout->outgoing_count)return XR_XIR_RUN_BAD_ARTIFACT;
        for(uint32_t p=0;p<op->args[1];++p)
            state->arguments[p]=vm_value_operand(run,run->function->operands[op->args[0]+p]);
        status=xr_xir_fixed_array_new(op->type,state->arguments,op->args[1],admission,&output);
    } else {
        XrXirValue receiver=vm_value_operand(run,op->args[0]);
        status=xr_xir_value_admit(&receiver,xr_xir_operand_type(run->function,op->args[0]),admission);
        if(status==XR_XIR_VALUE_OK) {
            if(op->op==XR_XIR_FIXED_ARRAY_GET)status=xr_xir_fixed_array_get(&receiver,
                vm_value_operand(run,op->args[1]).payload,&output,&fault);
            else {
                int64_t count=0;status=xr_xir_fixed_array_count(&receiver,&count);
                if(status==XR_XIR_VALUE_OK)output=(XrXirValue){XR_XIR_I64,0,count};
            }
        }
    }
    if(status==XR_XIR_VALUE_BOUNDS) {
        *action=(XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,XR_XIR_CALL_BOUNDS},
            .panic={.detail=fault}};return XR_XIR_RUN_OK;
    }
    if(status!=XR_XIR_VALUE_OK)return value_run_status(status);
    if(op->op==XR_XIR_FIXED_ARRAY_NEW)xr_xir_owned_slot_move(run->frame,destination,&output);
    else xr_xir_scalar_store(run->frame,destination,output.payload);
    return XR_XIR_RUN_OK;
}
