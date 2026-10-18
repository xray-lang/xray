/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_signature_cases.h - Literal signature projection and refusals
 */
#ifndef XIR_INVOCATION_SIGNATURE_CASES_H
#define XIR_INVOCATION_SIGNATURE_CASES_H

static XrXirStatus (*conditional_signature_reader)(const EffectBodyRequest *,uint32_t,
    EffectBodyFunctionHead *,EffectBodyFunctionDimensions *,EffectBodyFunctionSignature *);
static uint32_t conditional_signature_calls;
static bool conditional_signature_refused;
static XrXirStatus conditional_signature_delegate(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *head,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    ++conditional_signature_calls;
    if (conditional_signature_refused) return XR_XIR_BAD_STRUCTURE;
    return conditional_signature_reader(request,f,head,dimensions,signature);
}

/* These expectations are the literal Checked fixture, not another head read.
 * The same cases run after the generating input has been destroyed. */
static void conditional_compact_signatures(const XrXirCompileContext *context,const EffectBodyRequest *request) {
    const uint32_t counts[4]={0,2,1,0};
    const XrXirType results[4]={XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_UNIT};
    for (uint32_t f=0;f<4;++f) {
        EffectBodyFunctionSignature signature={0};XrCompileResourceStats before=rp_stats(context);
        CHECK(effect_body_request_signature(request,f,&signature)==XR_XIR_OK &&
            signature.parameter_count==counts[f] && signature.result==results[f] &&
            (!!signature.parameters==!!counts[f]));
        CHECK(rp_stats(context).work-before.work==11+sizeof(signature) &&
            rp_stats(context).allocation_count==before.allocation_count && rp_stats(context).live_bytes==before.live_bytes);
        if (f==1) CHECK(signature.parameters[0]==(XrXirType)257 && signature.parameters[1]==(XrXirType)256);
        if (f==2) CHECK(signature.parameters[0]==(XrXirType)256);
    }
    EffectBodyFunctionSignature sentinel={(const XrXirType *)(uintptr_t)1,77,XR_XIR_BOOL},output=sentinel;
    EffectBodyRequest rejected=*request;rejected.head=NULL;
    CHECK(effect_body_request_signature(&rejected,0,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&sentinel,sizeof(output)));
    CHECK(effect_body_request_signature(request,4,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&sentinel,sizeof(output)));
    CHECK(effect_body_request_signature(NULL,0,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&sentinel,sizeof(output)));
    CHECK(effect_body_request_signature(request,0,NULL)==XR_XIR_BAD_STRUCTURE);
    ConditionalCompactRead refusal={context,0,0};rejected=*request;
    rejected.read=(EffectBodyRead){&refusal,conditional_compact_read_work};
    CHECK(effect_body_request_signature(&rejected,0,&output)==XR_XIR_BUDGET && refusal.calls==1 &&
        !memcmp(&output,&sentinel,sizeof(output)));
    for (uint32_t mask=0;mask<8;++mask) {
        if (mask==1 || mask==2 || mask==4) continue;
        EffectBodyFunctionHead head={0};EffectBodyFunctionDimensions dimensions={77,78};
        XrCompileResourceStats before=rp_stats(context);
        CHECK(request->head(request,0,mask&1?&head:NULL,mask&2?&dimensions:NULL,mask&4?&output:NULL)==
            XR_XIR_BAD_STRUCTURE && !head.name && !head.instruction_count &&
            dimensions.parameter_count==77 && dimensions.instruction_count==78 && !memcmp(&output,&sentinel,sizeof(output)));
        CHECK(rp_stats(context).work==before.work && rp_stats(context).allocation_count==before.allocation_count);
    }
    rejected=*request;conditional_signature_reader=request->head;rejected.head=conditional_signature_delegate;
    conditional_signature_calls=0;conditional_signature_refused=false;
    CHECK(effect_body_request_signature(&rejected,1,&output)==XR_XIR_OK && conditional_signature_calls==1 &&
        output.parameter_count==2 && output.result==XR_XIR_I64 &&
        output.parameters[0]==(XrXirType)257 && output.parameters[1]==(XrXirType)256);
    output=sentinel;conditional_signature_refused=true;
    CHECK(effect_body_request_signature(&rejected,1,&output)==XR_XIR_BAD_STRUCTURE && conditional_signature_calls==2 &&
        !memcmp(&output,&sentinel,sizeof(output)));
    conditional_signature_reader=NULL;conditional_signature_refused=false;
}
/* The count oracle is literal Checked input, independent of both lifetime
 * callbacks. Partial requests retain every original owned head refusal. */
static void conditional_compact_dimensions(const XrXirCompileContext *context,
    const EffectBodyRequest *request) {
    const uint32_t parameters[4]={0,2,1,0},instructions[4]={5,2,2,3};
    for (uint32_t f=0;f<4;++f) {
        EffectBodyFunctionDimensions dimensions={77,78};
        CHECK(effect_body_request_dimensions(request,f,&dimensions)==XR_XIR_OK &&
            dimensions.parameter_count==parameters[f] && dimensions.instruction_count==instructions[f]);
    }
    EffectBodyFunctionDimensions sentinel={77,78},dimensions=sentinel;
    CHECK(effect_body_request_dimensions(request,4,&dimensions)==XR_XIR_BAD_STRUCTURE &&
        dimensions.parameter_count==77 && dimensions.instruction_count==78);
    EffectBodyRequest rejected=*request;rejected.head=NULL;
    CHECK(effect_body_request_dimensions(&rejected,0,&dimensions)==XR_XIR_BAD_STRUCTURE &&
        dimensions.parameter_count==77 && dimensions.instruction_count==78);
    ConditionalCompactRead refusal={context,0,0};rejected=*request;
    rejected.read=(EffectBodyRead){&refusal,conditional_compact_read_work};
    CHECK(effect_body_request_dimensions(&rejected,0,&dimensions)==XR_XIR_BUDGET &&
        dimensions.parameter_count==77 && dimensions.instruction_count==78 && refusal.calls==1);
}

static void conditional_compact_dimension_damage(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
    EffectBodyFunction *function=&certificate->body->functions[0],saved=*function;
    for (uint32_t damage=0;damage<7;++damage) {
        if (!damage) function->name=NULL;
        else if (damage==1) { function->parameter_count=1;function->parameters=NULL; }
        else if (damage==2) function->blocks=NULL;
        else if (damage==3) function->operands=NULL;
        else if (damage==4) function->runs=NULL;
        else if (damage==5) function->run_capacity=function->run_count-1;
        else function->run_count=0;
        EffectBodyFunctionDimensions dimensions={77,78};EffectBodyFunctionHead head={0};
        CHECK(effect_body_request_dimensions(&request,0,&dimensions)==XR_XIR_BAD_STRUCTURE &&
            dimensions.parameter_count==77 && dimensions.instruction_count==78);
        CHECK(effect_body_request_head(&request,0,&head)==XR_XIR_BAD_STRUCTURE && !head.name && !head.instruction_count);
        EffectBodyFunctionSignature signature={(const XrXirType *)(uintptr_t)1,77,XR_XIR_BOOL};
        CHECK(effect_body_request_signature(&request,0,&signature)==XR_XIR_BAD_STRUCTURE &&
            signature.parameters==(const XrXirType *)(uintptr_t)1 && signature.parameter_count==77 && signature.result==XR_XIR_BOOL);
        *function=saved;conditional_compact_dimensions(context,&request);
    }
}

#endif // XIR_INVOCATION_SIGNATURE_CASES_H
