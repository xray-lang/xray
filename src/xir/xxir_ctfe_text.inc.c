/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_text.inc.c - Prepaid deterministic scalar text through the common owner
 *
 * KEY CONCEPT:
 *   Only scalar_text chooses bytes. IEEE exponent bounds pay decimal work
 *   before formatting, while fixed local workspaces consume remaining frame.
 */
#include "xxir_output.h"
#include "xxir_float.h"
#include "xxir_text_cost.h"
#include "xxir_format.h"
#include "../shared/xr_decimal_float.h"
_Static_assert(sizeof(XrDecimalInteger)<=1024,"scalar text work reserve requires a bounded decimal workspace");
static XrXirCtfeStatus ctfe_text_reserve(ScalarRun *run,CtfeOwnedState *state,const XrXirValue *value) {
    if (!scalar_compile_work(run,64)) return XR_XIR_CTFE_BUDGET;
    XirTextCost cost=xir_text_cost(value);
    if (cost.frame>state->frame_spare) return XR_XIR_CTFE_FRAME_LIMIT;
    return scalar_compile_work(run,cost.work)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
}
static XrXirCtfeStatus ctfe_text_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    XrXirType type=xr_xir_operand_type(run->function,op->args[0]);
    XrXirValue input={0},result={0};
    if (type!=XR_XIR_UNIT && !xr_xir_value_from_payload(state->arena,type,
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]),&input)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!ctfe_scalar_type(type) && type!=XR_XIR_UNIT) {
        if (state->frame_spare<16384) return XR_XIR_CTFE_FRAME_LIMIT;
        CtfeOwnedState bounded=*state;bounded.frame_spare-=16384;
        XrXirValueAdmission admission=ctfe_value_admission(&bounded);
        XrXirCtfeStatus status=ctfe_value_status(xr_xir_value_format_string(&input,&admission,&result));
        if (status==XR_XIR_CTFE_OK) {
            xr_xir_owned_slot_move(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],&result);
            ++*cursor;
        }
        xr_xir_value_drop(&result);return status;
    }
    XrXirCtfeStatus status=ctfe_text_reserve(run,state,&input);if (status!=XR_XIR_CTFE_OK) return status;
    char buffer[XR_XIR_SCALAR_TEXT_BYTES];const char *bytes=NULL;size_t length=0;
    if (!xr_xir_scalar_text(&input,buffer,&bytes,&length) || length>sizeof(buffer)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!scalar_compile_work(run,(uint64_t)length)) return XR_XIR_CTFE_BUDGET;
    status=ctfe_value_status(xr_xir_string_new(state->domain,bytes,length,&result));
    if (status==XR_XIR_CTFE_OK) {
        xr_xir_owned_slot_move(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],&result);
        ++*cursor;
    }
    xr_xir_value_drop(&result);return status;
}
