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
static XrXirStatus effect_body_current_open(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,EffectBodyCurrentRead *output) {
    if(!output || output->work || output->material) return XR_XIR_BAD_STRUCTURE;
    EffectRoleCheck check={.work=work,.material=material};
    XrXirStatus status=effect_role_frame(&check);if(status!=XR_XIR_OK) return status;
    EffectBodyRead read=(EffectBodyRead){.owner=work};
    for(uint32_t f=0;f<check.functions;++f) {
        const EffectBodyFunction *head=&material->functions[f];
        if(!!head->runs!=!!head->run_capacity || !!head->run_count!=!!head->instruction_count ||
            head->run_capacity>head->instruction_count) return XR_XIR_BAD_STRUCTURE;
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        for(uint32_t r=0;r<head->run_count;++r) {
            const EffectBodyRun *ref=NULL;status=effect_body_range_ref(&read,head,&cursor,&ref);
            if(status!=XR_XIR_OK) return status;
            uint32_t count=(ref->mask&UINT32_C(1))?ref->count:1;
            for(uint32_t i=0;i<count;++i) {
                uint32_t opcode=XR_XIR_INVALID;
                status=effect_body_run_word(&read,ref,ref->first+i,0,&opcode);
                if(status!=XR_XIR_OK) return status;
                if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            }
        }
        if(cursor.first!=head->instruction_count || cursor.run!=head->run_count) return XR_XIR_BAD_STRUCTURE;
    }

    *output=(EffectBodyCurrentRead){work,material};return XR_XIR_OK;
}

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
