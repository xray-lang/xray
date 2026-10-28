/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_panic_value.inc.c - Compiler-local immutable panic observations
 *
 * KEY CONCEPT:
 *   The ordinary Value factory owns assertion messages. Intrinsic messages
 *   use the sole canonical formatter and preserve actual primitive facts.
 */
_Static_assert(sizeof(XrXirPanicPayload)<=80,"panic payload reserve must cover its public carrier");
_Static_assert(XR_XIR_PANIC_MESSAGE_CAPACITY<=128,"panic message reserve must cover canonical formatting");
static XrXirCtfeStatus ctfe_panic_value_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    if (ctfe_frame_spare(state)<512) return XR_XIR_CTFE_FRAME_LIMIT;
    XrXirValue info=ctfe_nominal_operand(run,op->args[0]),result={0};XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    if (op->op==XR_XIR_PANIC_CODE) {
        XrXirFaultDetail detail={0};
        if (!xr_xir_panic_info_detail(&info,&detail)) return XR_XIR_CTFE_BAD_ARTIFACT;
        result=(XrXirValue){XR_XIR_I64,0,detail.code};
    } else status=ctfe_value_status(xr_xir_panic_info_message(&info,&result));
    if (status==XR_XIR_CTFE_OK) {
        uint32_t offset=run->layout->offsets[run->function->parameter_count+vm->instruction];
        if (op->op==XR_XIR_PANIC_MESSAGE) xr_xir_owned_slot_move(run->frame,offset,&result);
        else xr_xir_scalar_store(run->frame,offset,result.payload);
        ++vm->instruction;
    }
    xr_xir_value_drop(&result);return status;
}
