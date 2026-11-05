/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_text.inc.c - Prepaid deterministic scalar text through the common owner
 *
 * KEY CONCEPT:
 *   Only scalar_text chooses bytes. Fixed decimal workspaces remain bounded
 *   by the current frame, including finite nonzero IEEE formatting.
 */
#include "xxir_output.h"
#include "xxir_float.h"
#include "../shared/xr_decimal_float.h"
_Static_assert(sizeof(XrDecimalInteger)<=1024,"scalar text requires a bounded decimal workspace");
static XrXirCtfeStatus ctfe_text_reserve(CtfeOwnedState *state,const XrXirValue *value) {
    uint64_t frame=512;uint32_t bits=xr_xir_float_bits((XrXirType)value->type);
    if (bits) {
        unsigned fraction=bits==32?23:52;uint64_t raw=(uint64_t)value->payload;
        uint64_t maximum=bits==32?255:2047,exponent=(raw>>fraction)&maximum;
        uint64_t magnitude=raw&((UINT64_C(1)<<(bits-1))-1);
        if (magnitude && exponent!=maximum) frame=16384;
    }
    return frame>ctfe_frame_spare(state)?XR_XIR_CTFE_FRAME_LIMIT:XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_text_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    XrXirValue input={(uint32_t)xr_xir_operand_type(run->function,op->args[0]),0,
        xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
    XrXirCtfeStatus status=ctfe_text_reserve(state,&input);if (status!=XR_XIR_CTFE_OK) return status;
    char buffer[XR_XIR_SCALAR_TEXT_BYTES];const char *bytes=NULL;size_t length=0;
    if (!xr_xir_scalar_text(&input,buffer,&bytes,&length) || length>sizeof(buffer)) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirValue result={0};status=ctfe_value_status(xr_xir_string_new(state->domain,bytes,length,&result));
    if (status==XR_XIR_CTFE_OK) {
        xr_xir_owned_slot_move(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],&result);
        ++*cursor;
    }
    xr_xir_value_drop(&result);return status;
}
