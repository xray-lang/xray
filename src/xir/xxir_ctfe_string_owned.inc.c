/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_string_owned.inc.c - Compiler-billed ordinary owned String operations
 *
 * Planning pays the same bounded search model before inspecting bytes. The
 * subsequent Value operation receives its complete work allowance in advance.
 */
#include "../shared/xr_string_core.h"
#include "xxir_string_owned_cost.h"
static XrXirValue ctfe_owned_string_operand(const ScalarRun *run,uint32_t id) {
    return (XrXirValue){XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[id])};
}
static XrXirCtfeStatus ctfe_owned_string_work(ScalarRun *run,const XrXirInstruction *op,
    const XrXirValue *arguments) {
    const char *text=NULL,*search=NULL,*replacement=NULL;size_t length=0,pattern=0,replaced=0;
    if(!xr_xir_string_view(arguments,&text,&length)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if(op->op==XR_XIR_STRING_COPY_BYTES) {
        if(length==UINT64_MAX || !scalar_compile_work(run,(uint64_t)length+1)) return XR_XIR_CTFE_BUDGET;
        return XR_XIR_CTFE_OK;
    }
    if(!xr_xir_string_view(arguments+1,&search,&pattern) ||
        !xr_xir_string_view(arguments+2,&replacement,&replaced)) return XR_XIR_CTFE_BAD_ARTIFACT;
    bool all=op->immediate!=0;
    uint64_t calls=all && pattern && pattern<=length?(uint64_t)(length/pattern)+2:1,planning=0;
    if(!xir_string_owned_search_work(length,pattern,calls,all?2:1,&planning) ||
        !xir_string_owned_add_work(&planning,pattern,1) || !scalar_compile_work(run,planning)) return XR_XIR_CTFE_BUDGET;
    XrStringCoreReplacePlan plan=xr_string_core_replace_plan(text,length,search,pattern,replacement,replaced,all);
    if(plan.kind==XR_STRING_CORE_REPLACE_INVALID || plan.len>INT64_MAX) return XR_XIR_CTFE_BUDGET;
    uint64_t execution=planning,fill=0;
    if(plan.kind==XR_STRING_CORE_REPLACE_ALLOC &&
        (!xir_string_owned_search_work(length,pattern,plan.count,1,&fill) ||
         !xir_string_owned_add_work(&fill,plan.len,1) || !xir_string_owned_add_work(&execution,fill,1)))
        return XR_XIR_CTFE_BUDGET;
    return scalar_compile_work(run,execution)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
}
static XrXirCtfeStatus ctfe_string_owned_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    bool copy=op->op==XR_XIR_STRING_COPY_BYTES;
    const uint32_t *ids=copy?op->args:run->function->operands+op->args[0];
    XrXirValue arguments[3]={0},result={0};
    for(uint32_t p=0;p<(copy?1u:3u);++p) arguments[p]=ctfe_owned_string_operand(run,ids[p]);
    XrXirCtfeStatus status=ctfe_owned_string_work(run,op,arguments);
    if(status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);
    XrXirValueStatus value=copy?xr_xir_string_copy_bytes(arguments,op->type,&admission,&result):
        xr_xir_string_replace(arguments,arguments+1,arguments+2,op->immediate!=0,&admission,&result);
    if(value==XR_XIR_VALUE_OK) {
        uint32_t destination=run->layout->offsets[run->function->parameter_count+*cursor];
        xr_xir_owned_slot_move(run->frame,destination,&result);++*cursor;
    }
    xr_xir_value_drop(&result);return ctfe_value_status(value);
}
