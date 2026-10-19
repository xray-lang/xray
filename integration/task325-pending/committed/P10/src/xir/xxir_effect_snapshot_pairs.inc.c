/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_pairs.inc.c - Literal pairs and complete affine promotion
 *
 * KEY CONCEPT:
 *   A two-row record stores the complete second physical row. A third row
 *   promotes it only after all ten words prove the same unsigned progression.
 */
static XrXirStatus effect_body_pair_extend(const XrXirCompileContext *work,
    EffectBodyRun *run,const XrXirInstruction *next,bool *extended) {
    if (!run || !next || !extended || run->count!=2 || run->mask!=EFFECT_BODY_MASK)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*extended)+sizeof(uint32_t))) return XR_XIR_BUDGET;
    *extended=false;uint32_t differences[EFFECT_BODY_WORDS],mask=0,active=0;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        /* Three physical reads, one scratch write and bounded arithmetic. */
        if (!xir_compile_work(work,20)) return XR_XIR_BUDGET;
        uint32_t first=effect_body_source_word(&run->base,w),second=run->delta[w];
        uint32_t difference=second-first;
        if (effect_body_source_word(next,w)!=second+difference) return XR_XIR_OK;
        differences[w]=difference;
        if (difference) { mask|=UINT32_C(1)<<w;++active; }
    }
    uint64_t work_units=EFFECT_BODY_WORDS+2*sizeof(uint32_t)+sizeof(*extended)+
        (uint64_t)active*2*sizeof(uint32_t);
    if (!xir_compile_work(work,work_units)) return XR_XIR_BUDGET;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w)
        if (mask&(UINT32_C(1)<<w)) run->delta[w]=differences[w];
    run->mask=mask;run->count=3;*extended=true;return XR_XIR_OK;
}

static XrXirStatus effect_body_pair_compare(const EffectBodyRead *read,
    const EffectBodyRun *ref,const XrXirFunction *source,bool *same) {
    if (!read || !ref || !source || !source->instructions || ref->count!=2 ||
        ref->mask!=EFFECT_BODY_MASK || ref->first>=source->instruction_count ||
        2>source->instruction_count-ref->first) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,6)) return XR_XIR_BUDGET;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if (!effect_body_read_work(read,7)) return XR_XIR_BUDGET;
        bool first=effect_body_source_word(&ref->base,w)!=
            effect_body_source_word(&source->instructions[ref->first],w);
        bool second=ref->delta[w]!=effect_body_source_word(&source->instructions[ref->first+1],w);
        if (first || second) {
            if (!same) return XR_XIR_BAD_STRUCTURE;
            if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
            *same=false;
        }
    }
    return XR_XIR_OK;
}
