/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_nullable.inc.c - Compiler-owned active payload operations
 *
 * KEY CONCEPT:
 *   Existing Value operations own each typed sum. Admission pays the complete
 *   finite type shape; equality prepays active String spans before comparison.
 */
#include "xxir_equal.h"
static bool ctfe_nullable_opcode(const XrXirInstruction *op) {
    return op->op==XR_XIR_NULLABLE_NONE || op->op==XR_XIR_NULLABLE_SOME ||
        op->op==XR_XIR_NULLABLE_IS_SOME || op->op==XR_XIR_NULLABLE_UNWRAP;
}
static XrXirCtfeStatus ctfe_nullable_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];XrXirValue result={0};
    bool create=op->op==XR_XIR_NULLABLE_NONE || op->op==XR_XIR_NULLABLE_SOME;
    XrXirType type=create?op->type:xr_xir_operand_type(run->function,op->args[0]);
    XrXirCtfeStatus status=ctfe_validation_work(run,state,type);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain,
        .work=UINT64_MAX,.scratch_bytes=state->frame_spare};
    XrXirValueStatus value_status=XR_XIR_VALUE_OK;
    if (create) {
        XrXirValue payload={0};
        if (op->op==XR_XIR_NULLABLE_SOME) {
            XrXirType element=xr_xir_nullable_element(run->module->types,type);
            if (element!=XR_XIR_UNIT) payload=(XrXirValue){(uint32_t)element,0,
                xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        }
        status=ctfe_admission_work(run,state,&payload);if (status!=XR_XIR_CTFE_OK) return status;
        value_status=xr_xir_nullable_new(type,op->op==XR_XIR_NULLABLE_SOME?&payload:NULL,&admission,&result);
    } else {
        XrXirValue receiver={(uint32_t)type,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        {
            bool some=false;const XrXirValue *payload=NULL;
            if (!xr_xir_nullable_view(&receiver,&some,&payload)) return XR_XIR_CTFE_BAD_ARTIFACT;
            if (op->op==XR_XIR_NULLABLE_IS_SOME) result=(XrXirValue){XR_XIR_BOOL,0,op->immediate?!some:some};
            else {
                if (!some) return XR_XIR_CTFE_NULL_UNWRAP;
                value_status=xr_xir_value_copy(payload,&result);
            }
        }
    }
    status=ctfe_value_status(value_status);
    if (status==XR_XIR_CTFE_OK && op->type!=XR_XIR_UNIT) {
        uint32_t offset=run->layout->offsets[run->function->parameter_count+*cursor];
        if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,offset,&result);
        else xr_xir_scalar_store(run->frame,offset,result.payload);
    }
    xr_xir_value_drop(&result);if (status==XR_XIR_CTFE_OK) ++*cursor;return status;
}
