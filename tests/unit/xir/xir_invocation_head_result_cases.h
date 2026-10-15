/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_head_result_cases.h - Current result reads retain head refusal boundaries
 */
#ifndef XIR_INVOCATION_HEAD_RESULT_CASES_H
#define XIR_INVOCATION_HEAD_RESULT_CASES_H

static XrXirStatus conditional_head_result_refuse(const EffectBodyRequest *request,uint32_t function,
    EffectBodyFunctionHead *head,EffectBodyFunctionDimensions *dimensions,XrXirType *result) {
    (void)request;(void)function;(void)head;(void)dimensions;(void)result;
    return XR_XIR_BAD_STRUCTURE;
}

/* The genuine entry returns literal I64 before and after producer death.
 * A refused read cannot publish a type, charge a retry or fall back to raw IR. */
static void conditional_head_result_literals(const XrXirCompileContext *context,
    const EffectBodyRequest *request) {
    XrXirType result=XR_XIR_BOOL;XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
    CHECK(effect_body_request_result(request,0,&result)==XR_XIR_OK && result==XR_XIR_I64);
    CHECK(rp_stats(context).work-before.work==11+sizeof(result) && rp_attempts==attempts &&
        rp_stats(context).allocation_count==before.allocation_count && rp_stats(context).live_bytes==before.live_bytes);
    before=rp_stats(context);result=XR_XIR_BOOL;
    CHECK(effect_body_request_result(request,4,&result)==XR_XIR_BAD_STRUCTURE && result==XR_XIR_BOOL);
    CHECK(effect_body_request_result(request,0,NULL)==XR_XIR_BAD_STRUCTURE);
    EffectBodyRequest refused=*request;refused.head=NULL;
    CHECK(effect_body_request_result(&refused,0,&result)==XR_XIR_BAD_STRUCTURE && result==XR_XIR_BOOL);
    refused=*request;refused.owner=NULL;
    CHECK(effect_body_request_result(&refused,0,&result)==XR_XIR_BAD_STRUCTURE && result==XR_XIR_BOOL);
    refused=*request;refused.domain.types=NULL;
    CHECK(effect_body_request_result(&refused,0,&result)==XR_XIR_BAD_STRUCTURE && result==XR_XIR_BOOL);
    refused=*request;refused.head=conditional_head_result_refuse;
    CHECK(effect_body_request_result(&refused,0,&result)==XR_XIR_BAD_STRUCTURE && result==XR_XIR_BOOL);
    ConditionalCompactRead budget={context,0,0};refused=*request;
    refused.read=(EffectBodyRead){&budget,conditional_compact_read_work};
    CHECK(effect_body_request_result(&refused,0,&result)==XR_XIR_BUDGET && result==XR_XIR_BOOL && budget.calls==1);
    EffectBodyFunctionHead head={0};EffectBodyFunctionDimensions dimensions={77,78};
    CHECK(request->head(request,0,&head,&dimensions,&result)==XR_XIR_BAD_STRUCTURE && result==XR_XIR_BOOL &&
        !head.name && dimensions.parameter_count==77 && dimensions.instruction_count==78);
    CHECK(request->head(request,0,NULL,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(rp_stats(context).work==before.work && rp_attempts==attempts);
    CHECK(effect_body_request_result(request,0,&result)==XR_XIR_OK && result==XR_XIR_I64);
}

#endif
