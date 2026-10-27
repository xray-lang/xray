/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_string_owned.inc.c - Compiler-owned ordinary String operations
 *
 * Value performs the actual search and failure-atomic receiving allocation.
 * Compiler memory and execution resources remain independent.
 */
#include "../shared/xr_string_core.h"
static XrXirValue ctfe_owned_string_operand(const ScalarRun *run,uint32_t id) {
    return (XrXirValue){XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[id])};
}

static XrXirCtfeStatus ctfe_string_owned_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    bool copy=op->op==XR_XIR_STRING_COPY_BYTES;
    const uint32_t *ids=copy?op->args:run->function->operands+op->args[0];
    XrXirValue arguments[3]={0},result={0};
    for(uint32_t p=0;p<(copy?1u:3u);++p) arguments[p]=ctfe_owned_string_operand(run,ids[p]);
    XrXirValueAdmission admission=ctfe_value_admission(state);
    XrXirValueStatus value=copy?xr_xir_string_copy_bytes(arguments,op->type,&admission,&result):
        xr_xir_string_replace(arguments,arguments+1,arguments+2,op->immediate!=0,&admission,&result);
    if(value==XR_XIR_VALUE_OK) {
        uint32_t destination=run->layout->offsets[run->function->parameter_count+*cursor];
        xr_xir_owned_slot_move(run->frame,destination,&result);++*cursor;
    }
    xr_xir_value_drop(&result);return ctfe_value_status(value);
}
