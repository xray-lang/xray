/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_current_input_read.inc.c - Scoped current canonical Input reads
 *
 * KEY CONCEPT:
 *   One allocation-free request frames every actual run before borrowing it.
 *   Subsequent random projections read current words without reopening the
 *   complete directory. The stack view never crosses an allocator callback.
 */

/* The caller supplies only a live stack view produced in this same request.
 * Compile work has no external callback, and all consuming clauses are read-only.
 * No copied instruction, directory or permission inventory is kept in the view. */
static XrXirStatus effect_body_current_record(const EffectBodyRequest *request,
    uint32_t function,uint32_t instruction,const EffectBodyRun **output) {
    const EffectBodyCurrentRead *current=request?request->current:NULL;
    if(!current || !output || current->material!=request->owner || current->work!=request->read.owner ||
        request->read.execution != NULL || !current->material ||
        function>=effect_body_count(current->material)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&current->material->functions[function];
    if(instruction>=head->instruction_count || !head->run_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t low=0,high=head->run_count;
    while(low<high) {

        uint32_t middle=low+(high-low)/2;
        if(head->runs[middle].first<=instruction) low=middle+1;else high=middle;
    }
    if(!low) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref=&head->runs[low-1];
    if(instruction<ref->first || instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;

    *output=ref;return XR_XIR_OK;
}
