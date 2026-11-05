/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_error_value.inc.c - Authentic internal enum error receiving
 *
 * KEY CONCEPT:
 *   Erasure retains the real enum. Complete metadata and owner admission
 *   precede the Value operations that inspect the active error graph.
 */
#include "xxir_error.h"
static XrXirCtfeStatus ctfe_error_concrete(ScalarRun *run,
    const XrXirValue *value,XrXirValue *concrete) {
    if (value->type==XR_XIR_ERROR) {
        if (!xr_xir_error_borrow(value,concrete)) return XR_XIR_CTFE_BAD_ARTIFACT;
    } else *concrete=*value;
    return xr_xir_type_is_enum(run->module->types,(XrXirType)concrete->type)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BAD_ARTIFACT;
}
static XrXirCtfeStatus ctfe_error_value_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    XrXirValue receiver=ctfe_nominal_operand(run,op->args[0]),result={0};
    XrXirCtfeStatus status=ctfe_value_graph_admit(run,state,&receiver);if (status!=XR_XIR_CTFE_OK) return status;
    if (op->op!=XR_XIR_ERROR_ERASE) {
        XrXirType type=op->op==XR_XIR_ERROR_IS?(XrXirType)op->immediate:op->type;
        status=ctfe_value_type_admit(run,state,type);if (status!=XR_XIR_CTFE_OK) return status;
    }
    XrXirValueAdmission admission=ctfe_value_admission(state);XrXirValueStatus received;
    if (op->op==XR_XIR_ERROR_ERASE) received=xr_xir_error_erase(&receiver,&admission,&result);
    else if (op->op==XR_XIR_ERROR_NARROW) received=xr_xir_error_narrow(&receiver,op->type,&admission,&result);
    else {
        bool matches=false;received=xr_xir_error_is(&receiver,(XrXirType)op->immediate,&admission,&matches);
        result=(XrXirValue){XR_XIR_BOOL,0,matches};
    }
    status=ctfe_value_status(received);
    if (status==XR_XIR_CTFE_OK) {
        uint32_t offset=run->layout->offsets[run->function->parameter_count+vm->instruction];
        if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,offset,&result);
        else xr_xir_scalar_store(run->frame,offset,result.payload);
        ++vm->instruction;
    }
    xr_xir_value_drop(&result);return status;
}
