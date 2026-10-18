/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_delta_group_scalar_cases.h - Independent original physical column scans
 */
#ifndef XIR_DELTA_GROUP_SCALAR_CASES_H
#define XIR_DELTA_GROUP_SCALAR_CASES_H

/* Independent original physical column scans. */
static XrXirStatus delta_original_run_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t index,uint32_t first,const EffectBodyRun **output) {
    if (!read || !output || !function || !function->runs ||
        function->run_count>function->run_capacity || function->run_count>function->instruction_count ||
        index>=function->run_count || first>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,7)) return XR_XIR_BUDGET;
    const EffectBodyRun *run=&function->runs[index];
    if (run->first!=first || !run->count || run->count>function->instruction_count-first ||
        run->mask&~EFFECT_BODY_MASK || (run->count==1 && run->mask)) return XR_XIR_BAD_STRUCTURE;
    /* A selected physical delta must be nonzero. The array is full length,
     * so there is no packed-column directory or rank to reconstruct. */
    for (uint32_t w=0;run->mask && w<EFFECT_BODY_WORDS;++w) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (!(run->mask&(UINT32_C(1)<<w))) continue;
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (!run->delta[w]) return XR_XIR_BAD_STRUCTURE;
    }
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=run;return XR_XIR_OK;
}

static XrXirStatus delta_original_instruction_ref_prepare(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction) {
    if (!read || !ref || (ref->mask&~EFFECT_BODY_MASK) || (ref->count==1 && ref->mask) ||
        instruction<ref->first || instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,1+sizeof(uint32_t)+((ref->mask&UINT32_C(1))?3:0)))
        return XR_XIR_BUDGET;
    uint32_t operation=effect_body_source_word(&ref->base,0)+((ref->mask&UINT32_C(1))?
        ref->delta[0]*(instruction-ref->first):0);
    if (operation<=XR_XIR_INVALID || operation>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    uint64_t units=EFFECT_BODY_WORDS+sizeof(XrXirInstruction)+2;
    for (uint32_t w=0;ref->mask && w<EFFECT_BODY_WORDS;++w) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (ref->mask&(UINT32_C(1)<<w)) {
            if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
            if (!ref->delta[w]) return XR_XIR_BAD_STRUCTURE;
            units+=4;
        }
    }
    if (!effect_body_read_work(read,units+EFFECT_BODY_WORDS)) return XR_XIR_BUDGET;
    return XR_XIR_OK;
}

#endif // XIR_DELTA_GROUP_SCALAR_CASES_H
