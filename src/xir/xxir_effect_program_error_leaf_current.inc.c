/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_error_leaf_current.inc.c - Current empty leaf outcomes
 *
 * KEY CONCEPT:
 *   A complete call-free body with only empty Error transfers has no escaping
 *   Error, even when another function has a real seed. Other bodies still need
 *   the general independent verifier and acquire no permission here.
 */
static XrXirStatus effect_program_error_leaves_current(const XrXirCompileContext *work,
    const XrXirEffects *effects) {
    const EffectBodyMaterial *body=effects->invocations->body;
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    EffectBodyRequest input=effect_body_owned_request(work,body);
    for (uint32_t f=0;f<effects->count;++f) {
        const EffectBodyFunction *head=&body->functions[f];
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};bool leaf=true;
        if (!xir_compile_work(work,sizeof(cursor)+sizeof(leaf)+5)) return XR_XIR_BUDGET;
        for (uint32_t r=0;r<head->run_count && leaf;++r) {
            const EffectBodyRun *run=NULL;
            XrXirStatus status=effect_body_range_ref(&input.read,head,&cursor,&run);
            if (status!=XR_XIR_OK) return status;
            uint32_t visits=run->mask&UINT32_C(1)?run->count:1;
            for (uint32_t offset=0;offset<visits;++offset) {
                uint32_t opcode=XR_XIR_INVALID;
                status=effect_body_run_word(&input.read,run,run->first+offset,0,&opcode);
                if (status!=XR_XIR_OK) return status;
                if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
                /* Cleanup can execute a deferred body even though its local
                 * instruction transfer has no Error seed. It is not a leaf. */
                if (!effect_physical_error_empty_opcode((XrXirOp)opcode) ||
                    opcode==XR_XIR_CLEANUP_REGISTER || opcode==XR_XIR_CLEANUP_LEAVE ||
                    opcode==XR_XIR_CLEANUP_ERROR) { leaf=false;break; }
            }
        }
        if (!leaf) continue;
        if (cursor.first!=head->instruction_count || cursor.run!=head->run_count)
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t word=0;word<final->words;++word) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (final->errors[(size_t)f*final->words+word]) return XR_XIR_BAD_STRUCTURE;
        }
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (final->functions[f].throws!=XR_XIR_EFFECT_NONE || final->task_errors[f]!=XR_XIR_OK)
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
