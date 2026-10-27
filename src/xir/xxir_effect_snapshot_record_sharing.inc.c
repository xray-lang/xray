/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_record_sharing.inc.c - Retained exact physical records
 *
 * KEY CONCEPT:
 *   Shared encoding carries no semantic product. The complete ten-word
 *   comparison precedes the same current-source semantic intake as writing.
 */
typedef struct EffectBodyRecordShareRequest {
    uint32_t function;
    const XrXirFunction *source;
    const EffectBodyWriteSink *sink;
} EffectBodyRecordShareRequest;

static XrXirStatus effect_body_records_share(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *ordinary,EffectBodyMaterial *body,EffectBodyMaterial *parent,
    const EffectBodyRecordShareRequest *request,bool *shared) {
    if (!xir_compile_context_valid(work) || !ordinary || !body || !request || !shared || *shared ||
        !effect_body_material_shape(parent) || parent==body || parent->resources!=work->resources ||
        body->resources!=work->resources || request->function>=effect_body_count(body) ||
        !request->source)
        return XR_XIR_BAD_STRUCTURE;

    uint32_t index=UINT32_MAX;
    XrXirStatus status=effect_body_ordinary_record_index(work,ordinary,request->function,&index);
    if (status!=XR_XIR_OK) return status;
    if (index>=effect_body_count(parent)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *previous=&parent->functions[index];
    EffectBodyFunction *target=&body->functions[request->function];
    if (target->runs || target->run_count || target->run_capacity || target->borrowed_runs)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=(EffectBodyRead){.owner=work};bool same=true;
    status=effect_body_records_compare(&read,previous,request->source,&same);
    if (status!=XR_XIR_OK || !same || !previous->run_count) return status;

    status=effect_body_runs_retain(work,previous);
    if (status!=XR_XIR_OK) return status;
    /* All aliases follow the successful lifetime acquisition. Even a later
     * semantic refusal destroys only this owner's independently built proof. */
    target->runs=previous->runs;target->run_count=previous->run_count;
    target->run_capacity=previous->run_capacity;target->borrowed_runs=true;
    for (uint32_t r=0;r<previous->run_count;++r) {

        status=effect_body_run_intake(request->source,&previous->runs[r],request->sink);
        if (status!=XR_XIR_OK) return status;
    }
    *shared=true;return XR_XIR_OK;
}
