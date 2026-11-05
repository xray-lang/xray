/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_final_callable_read.inc.c - Current callable capture completeness
 */
static XrXirStatus effect_final_callable_complete(const XrXirCompileContext *work,
    const EffectInvocationFinal *final,uint32_t node,uint32_t value,bool *complete) {
    if(!complete || !xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,final,1,sizeof(*final));if(status!=XR_XIR_OK) return status;
    if(!final || node>=final->count || final->count>final->capacity ||
        final->capture.count>final->capture.capacity || !!final->capture.facts!=!!final->capture.capacity)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,final->capture.facts,final->capture.capacity,sizeof(*final->capture.facts));
    if(status!=XR_XIR_OK) return status;
    EffectCaptureRead read={.work=work,.final=final};const EffectCaptureFact *fact=NULL;
    status=effect_capture_find(&read,
        (EffectCaptureFact){node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0},&fact);
    if(status==XR_XIR_OK) *complete=fact!=NULL;
    return status;
}
