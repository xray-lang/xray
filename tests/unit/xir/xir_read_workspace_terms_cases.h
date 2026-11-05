/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Query storage never becomes a published structural owner.
 */
#ifndef XIR_READ_WORKSPACE_TERMS_CASES_H
#define XIR_READ_WORKSPACE_TERMS_CASES_H
#include "xir_generic_method_owned_fixture.h"

static XrXirStatus mr_terms_attempt(const XrXirCompileContext *work,const EffectTerms *owner,
    const XrXirTypes *current,XirReadOperation *operation,bool *output) {
    EffectTerms terms={.types=owner->types,.remaining=work,.read=&operation->capacity};
    size_t allocations=attempts;uint64_t held=pc_stats(work).held_bytes;
    XrXirStatus status=effect_terms_snapshot_match(&terms,current,owner->types.count);
    CHECK(attempts==allocations && pc_stats(work).held_bytes==held);
    if(status==XR_XIR_OK) {
        CHECK(terms.memory && terms.match.memory && !effect_terms_storage_owned(work,&terms));
        /* Erasing the borrow tag cannot turn a payload inside prepared
         * capacity into an independently owned allocation. */
        terms.read=NULL;
        CHECK(!effect_terms_storage_owned(work,&terms));
        terms.read=&operation->capacity;
    }
    effect_terms_free(&terms);
    CHECK(!operation->capacity.active && attempts==allocations && pc_stats(work).held_bytes==held);
    if(status==XR_XIR_OK) *output=true;
    return status;
}
static void mr_terms_cases(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t empty=pc_stats(&context).held_bytes;
    GenericMethodOwnedFixture original,current;
    generic_method_owned_fixture(&original);generic_method_owned_fixture(&current);
    CHECK(xr_xir_compile_interfaces_verify_structure(&context,&original.table,&original.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_interfaces_verify_structure(&context,&current.table,&current.types)==XR_XIR_OK);
    CHECK(original.table.declarations!=current.table.declarations);
    EffectTerms owner={.remaining=&context};
    CHECK(effect_terms_seed(&owner,&original.types)==XR_XIR_OK);
    CHECK(effect_terms_storage_owned(&context,&owner));
    generic_method_owned_assert(owner.types.interfaces);
    uint64_t held=pc_stats(&context).held_bytes;
    for(unsigned action=0;action<2;++action) {
        XirReadOperation operation={.capacity={.resources=context.resources}};bool output=false;
        XrXirStatus status=XR_XIR_OK;size_t retries=0;
        for(;;) {
            CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
            status=mr_terms_attempt(&context,&owner,&current.types,&operation,&output);
            XirReadStep step=xir_read_step(&operation,status);
            if(step.kind!=XIR_READ_NEED_PREPARE) break;
            CHECK(!output && !operation.capacity.active);++retries;
            if(action) {fail_at=attempts;injected=false;}
            status=xir_read_prepare(&context,&operation,NULL);fail_at=SIZE_MAX;
            if(status!=XR_XIR_OK) break;
            for(XirReadScratchBlock *block=operation.capacity.memory;block;block=block->next)
                if(!block->clear) memset(block+1,0xD7,block->bytes);
        }
        if(action) CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && !output);
        else {
            CHECK(status==XR_XIR_OK && output && retries && operation.capacity.claims && operation.capacity.blocks);
            printf("Terms parent+own/cross-pool: retries%zu prepares%zu blocks%zu bytes%zu claims%zu clear%zu frame%llu peakHeld%llu\n",
                retries,operation.capacity.preparations,operation.capacity.blocks,operation.capacity.capacity,
                operation.capacity.claims,operation.capacity.clear_bytes,(unsigned long long)context.limits.frame_bytes,
                (unsigned long long)pc_stats(&context).peak_bytes);
        }
        xir_read_operation_free(&operation);CHECK(pc_stats(&context).held_bytes==held);
        CHECK(effect_terms_storage_owned(&context,&owner));generic_method_owned_assert(owner.types.interfaces);
    }
    /* The semantic owner survives retirement of every query capacity block. */
    CHECK(effect_terms_snapshot_match(&owner,&current.types,owner.types.count)==XR_XIR_OK);
    effect_terms_free(&owner);pc_release(&context,empty);
    puts("Terms query: poisoned capacity, real base provenance, representative OOM, owner retirement, physical0");
}
#endif
