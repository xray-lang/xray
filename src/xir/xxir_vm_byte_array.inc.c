/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_byte_array.inc.c - Typed byte candidate execution
 *
 * KEY CONCEPT:
 *   Owned inputs remain frame-owned while one complete candidate is prepared.
 */
static XrXirRunStatus vm_byte_array_step(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,uint32_t destination,XrXirAction *action) {
    (void)state;
    if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
    bool repeat=op->op==XR_XIR_BYTE_ARRAY_REPEAT;
    const uint32_t *ids=repeat?run->function->operands+op->args[0]:op->args;
    XrXirValue array=vm_value_operand(run,ids[0]),output={0};
    XrXirFaultDetail fault={0};XrXirCallStatus status;
    if (repeat) status=xr_xir_instance_array_byte_repeat(run->view,&array,
        vm_value_operand(run,ids[1]).payload,vm_value_operand(run,ids[2]).payload,&output,&fault);
    else {
        XrXirValue snapshot=vm_value_operand(run,ids[1]);
        status=xr_xir_instance_array_byte_append(run->view,&array,&snapshot,&output);
    }
    if (status!=XR_XIR_CALL_READY) {
        *action=(XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,status},.panic={.detail=fault}};
        return XR_XIR_RUN_OK;
    }
    xr_xir_owned_slot_move(run->frame,destination,&output);return XR_XIR_RUN_OK;
}
