/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_read_workspace.c - Owned preparation, consumed ready and whole retry
 */
#include "xir/xxir_read_attempt.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)

static XrCompileResourceStats rw_stats(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static XrXirCompileContext rw_context(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={.limits={.frame_bytes=UINT64_C(16777216)}};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);return context;
}
static void rw_drop(XrXirCompileContext *context,XirReadOperation *operation,uint64_t held) {
    xir_read_operation_free(operation);CHECK(rw_stats(context->resources).held_bytes==held);
    xr_compile_resources_release(context->resources);context->resources=NULL;
}
static XrXirStatus rw_attempt(const XrXirCompileContext *context,XirReadOperation *operation,unsigned *output) {
    unsigned sum=0;
    for(unsigned function=0;function<3;++function) {
        size_t bytes=8193+function;XirReadClaim claim={0};XrXirStatus status=XR_XIR_OK;
        unsigned char *memory=xir_read_scratch_alloc(context,&operation->capacity,bytes,true,&claim,&status);
        if(!memory) return status;
        for(size_t i=0;i<bytes;++i) CHECK(memory[i]==0);
        memset(memory,0xA5,bytes);sum+=function+1;
        xir_read_scratch_release(&operation->capacity,memory,&claim);
    }
    *output=sum;return XR_XIR_OK;
}
static void rw_complete_retry(void) {
    XrXirCompileContext context=rw_context();uint64_t held=rw_stats(context.resources).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};unsigned output=0xC9;
    size_t retries=0;
    for(;;) {
        CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
        XirReadStep step=xir_read_step(&operation,rw_attempt(&context,&operation,&output));
        if(step.kind!=XIR_READ_NEED_PREPARE) {CHECK(step.kind==XIR_READ_COMPLETE && step.status==XR_XIR_OK);break;}
        CHECK(output==0xC9 && step.status==XR_XIR_OK);
        CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);CHECK(++retries<=3);
    }
    CHECK(output==6 && retries==3 && operation.capacity.blocks==3 && !operation.capacity.active);
    /* The entire second attempt starts at function zero and consumes freshly
     * prepared clear state, rather than treating dirty released bytes as zero. */
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);output=0xC9;
    XirReadStep step=xir_read_step(&operation,rw_attempt(&context,&operation,&output));
    CHECK(step.kind==XIR_READ_NEED_PREPARE && output==0xC9 && operation.capacity.missing==0);
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
    step=xir_read_step(&operation,rw_attempt(&context,&operation,&output));
    CHECK(step.kind==XIR_READ_COMPLETE && output==6 && operation.capacity.blocks==3);
    printf("WORKSPACE ready consumed: whole3function retries%zu blocks%zu capacity%zu clearBytes%zu claims%zu peakHeld%llu\n",
        retries,operation.capacity.blocks,operation.capacity.capacity,operation.capacity.clear_bytes,
        operation.capacity.claims,(unsigned long long)rw_stats(context.resources).peak_bytes);
    rw_drop(&context,&operation,held);
}
typedef struct ReadPrefixProbe {XirReadOperation *operation;size_t polls,witnesses;} ReadPrefixProbe;
static bool rw_cancel_prefix(const void *opaque) {
    ReadPrefixProbe *probe=(ReadPrefixProbe *)opaque;++probe->polls;
    XirReadScratchBlock *block=probe->operation->capacity.memory;
    if(!block || block->bytes!=8193) return false;
    const unsigned char *bytes=(const unsigned char *)(block+1);
    if(bytes[0] || bytes[4095] || bytes[4096]!=0xA5) return false;
    for(size_t i=0;i<4096;++i) CHECK(bytes[i]==0);
    for(size_t i=4096;i<8193;++i) CHECK(bytes[i]==0xA5);
    CHECK(!probe->operation->capacity.active);++probe->witnesses;return true;
}
static void rw_control_prefix(void) {
    XrXirCompileContext context=rw_context();uint64_t held=rw_stats(context.resources).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};
    XirReadClaim claim={0};XrXirStatus status=XR_XIR_OK;
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,8193,true,&claim,&status));
    CHECK(xir_read_step(&operation,status).kind==XIR_READ_NEED_PREPARE);
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    unsigned char *memory=xir_read_scratch_alloc(&context,&operation.capacity,8193,true,&claim,&status);
    CHECK(memory && status==XR_XIR_OK);memset(memory,0xA5,8193);
    xir_read_scratch_release(&operation.capacity,memory,&claim);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,8193,true,&claim,&status));
    ReadPrefixProbe probe={.operation=&operation};
    XrCompileRequestOptions options={.cancel_poll=rw_cancel_prefix,.cancel_context=&probe};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    status=xir_read_prepare(&context,&operation,&control);
    XirReadStep step=xir_read_step(&operation,status);
    CHECK(step.kind==XIR_READ_FAILED && step.status==XR_XIR_CANCELLED && probe.witnesses==1);
    CHECK(xr_compile_request_control_poll(&control)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(!operation.capacity.active && !operation.capacity.memory->ready);
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    memory=xir_read_scratch_alloc(&context,&operation.capacity,8193,true,&claim,&status);
    CHECK(memory && status==XR_XIR_OK);for(size_t i=0;i<8193;++i)CHECK(memory[i]==0);
    xir_read_scratch_release(&operation.capacity,memory,&claim);
    puts("WORKSPACE actual4096zero/4097dirty prefix cancellation, firstcause and same-capacity fresh retry PASS");
    rw_drop(&context,&operation,held);
}
static void rw_deadline(void) {
    XrXirCompileContext context=rw_context();uint64_t held=rw_stats(context.resources).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};XirReadClaim claim={0};XrXirStatus status=XR_XIR_OK;
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,9000,true,&claim,&status));
    XrCompileRequestOptions options={.deadline_mode=XR_COMPILE_DEADLINE_AFTER,.timeout_ns=0};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_read_prepare(&context,&operation,&control)==XR_XIR_DEADLINE);
    CHECK(!operation.capacity.memory && rw_stats(context.resources).held_bytes==held);
    rw_drop(&context,&operation,held);
}
static void rw_real_failures(void) {
    XrXirCompileContext context=rw_context();uint64_t held=rw_stats(context.resources).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};
    XirReadClaim claim={0};XrXirStatus status=XR_XIR_OK;
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,9000,true,&claim,&status));
    context.limits.frame_bytes=1024;
    status=xir_read_prepare(&context,&operation,NULL);
    XirReadStep step=xir_read_step(&operation,status);
    CHECK(step.kind==XIR_READ_FAILED && step.status==XR_XIR_BUDGET && !operation.capacity.memory);
    CHECK(rw_stats(context.resources).held_bytes==held);
    context.limits.frame_bytes=UINT64_C(16777216);
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    rw_drop(&context,&operation,held);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,held+1024,false};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    operation=(XirReadOperation){.capacity={.resources=context.resources}};
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,9000,true,&claim,&status));
    status=xir_read_prepare(&context,&operation,NULL);step=xir_read_step(&operation,status);
    CHECK(step.kind==XIR_READ_FAILED && step.status==XR_XIR_MEMORY_LIMIT && !operation.capacity.memory);
    rw_drop(&context,&operation,held);
    puts("WORKSPACE actual frame BUDGET and finite-held MEMORY_LIMIT remain failures, not preparation retries PASS");
}
static void rw_changed_clear_demand(void) {
    XrXirCompileContext context=rw_context();uint64_t held=rw_stats(context.resources).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};XirReadClaim claim={0};
    XrXirStatus status=XR_XIR_OK;
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,4097,false,&claim,&status));
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    unsigned char *memory=xir_read_scratch_alloc(&context,&operation.capacity,4097,false,&claim,&status);
    CHECK(memory && status==XR_XIR_OK);memset(memory,0xA5,4097);
    xir_read_scratch_release(&operation.capacity,memory,&claim);
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,1,false,&claim,&status));
    CHECK(operation.capacity.missing==1 && xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    CHECK(!operation.capacity.memory->clear && ((unsigned char *)(operation.capacity.memory+1))[0]==0xA5);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    /* Another parent attempt can change the storage role of this ordinal.
     * An unrelated prepare does not prove that a reserve-only slot is zero. */
    memory=xir_read_scratch_alloc(&context,&operation.capacity,4097,true,&claim,&status);
    CHECK(!memory && status==XR_XIR_BUDGET && operation.capacity.required==4097 && operation.capacity.clear);
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    memory=xir_read_scratch_alloc(&context,&operation.capacity,4097,true,&claim,&status);
    CHECK(memory && status==XR_XIR_OK);for(size_t i=0;i<4097;++i) CHECK(memory[i]==0);
    xir_read_scratch_release(&operation.capacity,memory,&claim);
    printf("WORKSPACE reserve-only to zeroed ordinal: blocks%zu prepares%zu clearBytes%zu claims%zu\n",
        operation.capacity.blocks,operation.capacity.preparations,operation.capacity.clear_bytes,operation.capacity.claims);
    rw_drop(&context,&operation,held);
}
static void rw_reused_current_frame(void) {
    XrXirStatus (*volatile begin_read)(const XrXirCompileContext *,XirReadOperation *)=xir_read_begin;
    void *(*volatile claim_read)(const XrXirCompileContext *,XirReadScratch *,size_t,bool,XirReadClaim *,XrXirStatus *)=xir_read_scratch_alloc;
    XrXirCompileContext context=rw_context();uint64_t held=rw_stats(context.resources).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};XirReadClaim claim={0};
    XrXirStatus status=XR_XIR_OK;
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);
    CHECK(!xir_read_scratch_alloc(&context,&operation.capacity,9000,false,&claim,&status));
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK);
    XrXirCompileContext narrow=context;narrow.limits.frame_bytes=operation.capacity.capacity;
    size_t claims=operation.capacity.claims,blocks=operation.capacity.blocks,capacity=operation.capacity.capacity;
    uint64_t reserved=rw_stats(context.resources).held_bytes;
    /* The same ledger does not grant a different caller the old frame policy.
     * This limit fits the payload but not its actual physical owner header. */
    volatile XrXirStatus observed=begin_read(&narrow,&operation);
    CHECK(observed==XR_XIR_BUDGET && !operation.capacity.required && !operation.capacity.active);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    CHECK(!claim_read(&narrow,&operation.capacity,1,false,&claim,&status) && status==XR_XIR_BUDGET);
    CHECK(!operation.capacity.required && !operation.capacity.active && !claim.memory && operation.capacity.claims==claims &&
        operation.capacity.blocks==blocks && operation.capacity.capacity==capacity && rw_stats(context.resources).held_bytes==reserved);
    CHECK(xir_read_begin(&context,&operation)==XR_XIR_OK);status=XR_XIR_OK;
    void *memory=xir_read_scratch_alloc(&context,&operation.capacity,9000,false,&claim,&status);
    CHECK(memory && status==XR_XIR_OK);xir_read_scratch_release(&operation.capacity,memory,&claim);
    rw_drop(&context,&operation,held);
    puts("WORKSPACE same-ledger narrower current frame: no NEED, no new claim, original policy recovery PASS");
}
int main(void) {
    rw_changed_clear_demand();rw_reused_current_frame();rw_complete_retry();rw_control_prefix();rw_deadline();rw_real_failures();
    puts("WORKSPACE independent preparation outcome, token ownership, original limits and resource rollback PASS");return 0;
}
