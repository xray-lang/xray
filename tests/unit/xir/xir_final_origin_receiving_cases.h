/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_final_origin_receiving_cases.h - Complete request reads and immutable origin evidence
 */
#ifndef XIR_FINAL_ORIGIN_RECEIVING_CASES_H
#define XIR_FINAL_ORIGIN_RECEIVING_CASES_H
/* A delegated test reader is exhausted before the authentic Final receiver.
 * Its status is observable, but none of its conclusions grants permission. */
static XrXirStatus final_origin_request_rows(const EffectBodyRequest *body) {
    for(uint32_t f=0;f<body->domain.function_count;++f) {
        EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(body,f,&head);
        if(status!=XR_XIR_OK) return status;
        EffectBodyRecordCursor cursor;status=effect_body_request_sequence_begin(body,&cursor);
        for(uint32_t i=0;status==XR_XIR_OK && i<head.instruction_count;++i) {
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_body_request_sequence_instruction(body,f,i,&cursor,&storage,&op);
        }
        if(status==XR_XIR_OK) status=effect_body_request_sequence_finish(body,f,&cursor);
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus final_origin_receive(const XrXirCompileContext *work,XrXirEffects *effects,
    const EffectBodyRequest *request) {
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationRead read={0};
    XrXirStatus status=request?final_origin_request_rows(request):XR_XIR_OK;
    if(status==XR_XIR_OK) status=effect_body_physical_match(work,certificate->final->physical,certificate->body);
    if(status==XR_XIR_OK) status=effect_role_proof_match(work,certificate->body,&certificate->final->role_proof);
    if(status==XR_XIR_OK) status=effect_capture_match(work,certificate->final);
    if(status==XR_XIR_OK) status=effect_invocation_read_open(work,certificate,certificate->final,&read);
    if(status==XR_XIR_OK) status=effect_invocation_iterators_read(&read,&read.body,NULL);
    if(status==XR_XIR_OK) status=effect_invocation_closed_read(work,certificate->final);
    effect_invocation_read_close(&read);return status;
}
#endif // XIR_FINAL_ORIGIN_RECEIVING_CASES_H
