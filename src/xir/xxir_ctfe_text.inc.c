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
#include "../shared/xr_decimal_float.h"
_Static_assert(sizeof(XrDecimalInteger)<=1024,"scalar text work reserve requires a bounded decimal workspace");
static XrXirCtfeStatus ctfe_text_reserve(ScalarRun *run,CtfeOwnedState *state,const XrXirValue *value) {
    if (!scalar_compile_work(run,64)) return XR_XIR_CTFE_BUDGET;
    uint64_t work=256,frame=512;uint32_t bits=xr_xir_float_bits((XrXirType)value->type);
    if (bits) {
        unsigned fraction=bits==32?23:52;uint64_t raw=(uint64_t)value->payload;
        uint64_t maximum=bits==32?255:2047,exponent=(raw>>fraction)&maximum;
        uint64_t magnitude=raw&((UINT64_C(1)<<(bits-1))-1);
        work=1024;
        if (magnitude && exponent!=maximum) {
            int bias=bits==32?127:1023;
            int power=exponent?(int)exponent-bias:1-bias-(int)fraction;
            uint64_t distance=(uint64_t)(power<0?-power:power);
            /* Binary32/64 decimal intermediates stay below 1200 bits. At most
             * 17 digits and 9 subtractions per digit fit the fixed reserve.
             * Normalization needs no more than |binary exponent|+2 iterations;
             * each pays full bigint copies and limb scans before common code. */
            work=UINT64_C(131072)+(distance+2)*UINT64_C(2048);frame=16384;
        }
    }
    if (frame>state->frame_spare) return XR_XIR_CTFE_FRAME_LIMIT;
    return scalar_compile_work(run,work)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
}
static XrXirCtfeStatus ctfe_text_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    XrXirValue input={(uint32_t)xr_xir_operand_type(run->function,op->args[0]),0,
        xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
    XrXirCtfeStatus status=ctfe_text_reserve(run,state,&input);if (status!=XR_XIR_CTFE_OK) return status;
    char buffer[XR_XIR_SCALAR_TEXT_BYTES];const char *bytes=NULL;size_t length=0;
    if (!xr_xir_scalar_text(&input,buffer,&bytes,&length) || length>sizeof(buffer)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!scalar_compile_work(run,(uint64_t)length)) return XR_XIR_CTFE_BUDGET;
    XrXirValue result={0};status=ctfe_value_status(xr_xir_string_new(state->domain,bytes,length,&result));
    if (status==XR_XIR_CTFE_OK) {
        xr_xir_owned_slot_move(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],&result);
        ++*cursor;
    }
    xr_xir_value_drop(&result);return status;
}
