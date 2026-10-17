/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_empty_current.inc.c - Current seedless Error equations
 *
 * KEY CONCEPT:
 *   A complete Input with only empty transfers and internal direct calls has
 *   the unique least empty solution, including recursive call components.
 *   Other Inputs still require the general independent Error verifier.
 */
static XrXirStatus effect_error_empty_current(const XrXirCompileContext *work,
    const XrXirEffects *effects,bool *handled) {
    if (!handled) return XR_XIR_BAD_STRUCTURE;
    *handled=false;
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if (!final || !effects->invocations || !effects->invocations->body)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *body=effects->invocations->body;
    EffectBodyRequest input=effect_body_owned_request(work,body);
    if (body->domain.function_count!=effects->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<effects->count;++f) {
        const EffectBodyFunction *head=&body->functions[f];
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        for (uint32_t r=0;r<head->run_count;++r) {
            const EffectBodyRun *run=NULL;
            XrXirStatus status=effect_body_range_ref(&input.read,head,&cursor,&run);
            if (status!=XR_XIR_OK) return status;
            uint32_t visits=run->mask&UINT32_C(1)?run->count:1;
            for (uint32_t offset=0;offset<visits;++offset) {
                uint32_t opcode=XR_XIR_INVALID;
                status=effect_body_run_word(&input.read,run,run->first+offset,0,&opcode);
                if (status!=XR_XIR_OK) return status;
                if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
                if (!effect_physical_error_empty_opcode((XrXirOp)opcode) &&
                    opcode!=XR_XIR_CALL && opcode!=XR_XIR_INVOKE &&
                    opcode!=XR_XIR_CALL_DEFAULT && opcode!=XR_XIR_INVOKE_DEFAULT)
                    return XR_XIR_OK;
            }
        }
        if (cursor.first!=head->instruction_count || cursor.run!=head->run_count)
            return XR_XIR_BAD_STRUCTURE;
    }
    /* Direct targets and default bindings were independently checked against
     * this complete Input. There is no external callee or escaping seed in
     * this domain; no proposed positive row can authorize itself in a cycle. */
    for (uint32_t row=0;row<final->error_rows;++row)
        for (uint32_t word=0;word<final->words;++word) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (final->errors[(size_t)row*final->words+word]) return XR_XIR_BAD_STRUCTURE;
        }
    for (uint32_t f=0;f<effects->count;++f) {
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (final->functions[f].throws!=XR_XIR_EFFECT_NONE || final->task_errors[f]!=XR_XIR_OK)
            return XR_XIR_BAD_STRUCTURE;
    }
    *handled=true;return XR_XIR_OK;
}
