/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_zero_cases.h - Fixed numeric state and drain oracles
 *
 * KEY CONCEPT:
 *   Two instances initialize one diamond independently and stop new admission before release.
 */
#ifndef XIR_MODULE_STATE_ZERO_CASES_H
#define XIR_MODULE_STATE_ZERO_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_panic.h"

typedef struct StateZeroShape {
    uint32_t advance, root, base, left, right, slot;
} StateZeroShape;
typedef struct StateZeroTrace {
    XrXirInstance *instance;
    uint32_t begins[4], ready[4], order[4], begin_count, ready_count;
    uint32_t published, released, busy;
} StateZeroTrace;
static StateZeroShape state_zero_shape(const XrXirDeclarations *d, uint32_t advance) {
    CHECK(d && d->module_count==4 && d->slot_count==1 && d->root_module<4);
    uint32_t root=d->root_module;
    CHECK(d->modules[root].dependency_count==2 && d->modules[root].dependencies);
    uint32_t left=d->modules[root].dependencies[0],right=d->modules[root].dependencies[1];
    CHECK(left<4 && right<4 && left!=right && left!=root && right!=root);
    CHECK(d->modules[left].dependency_count==1 && d->modules[left].dependencies);
    CHECK(d->modules[right].dependency_count==1 && d->modules[right].dependencies);
    uint32_t base=d->modules[left].dependencies[0];
    CHECK(base<4 && base!=root && base!=left && base!=right);
    CHECK(d->modules[right].dependencies[0]==base && !d->modules[base].dependency_count);
    CHECK(d->slots[0].module==base && d->slots[0].type==XR_XIR_ATOMIC_I64 && !d->slots[0].mutable);
    CHECK(d->functions[advance].module==root && d->functions[advance].exported &&
        !d->functions[advance].nominal_owner && !d->functions[advance].cleanup_owner);
    return (StateZeroShape){advance,root,base,left,right,0};
}
static void state_zero_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    StateZeroTrace *trace=context;
    CHECK(trace && trace->instance);
    /* An active observer holds the instance and its code lease. Destruction
     * and a new admission cannot consume that owner during the callback. */
    CHECK(xr_xir_instance_free(trace->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_start(trace->instance,UINT32_MAX,NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(trace->instance)==XR_XIR_CALL_BUSY);
    ++trace->busy;
    if(event==XR_XIR_MODULE_BEGIN) {
        CHECK(index<4 && trace->begin_count<4 && index==trace->order[trace->begin_count]);
        CHECK(!trace->begins[index]); ++trace->begins[index]; ++trace->begin_count;
    } else if(event==XR_XIR_MODULE_READY) {
        CHECK(index<4 && trace->ready_count<4 && index==trace->order[trace->ready_count]);
        CHECK(trace->begins[index] && !trace->ready[index]); ++trace->ready[index]; ++trace->ready_count;
    } else if(event==XR_XIR_SLOT_PUBLISHED) {
        CHECK(index==0 && !trace->published && trace->begins[trace->order[0]] && !trace->ready[trace->order[0]]);
        ++trace->published;
    }
    else { CHECK(event==XR_XIR_SLOT_RELEASED && index==0 && trace->published && !trace->released); ++trace->released; }
}
static void state_zero_pair(XrXirProgram *program,StateZeroShape shape) {
    CHECK(program && !runtime_live && !runtime_bytes);
    size_t compiler_blocks=source_program_compile_live,compiler_bytes=source_program_compile_bytes;
    CHECK(compiler_blocks>source_program_owner_count);
    StateZeroTrace traces[2]={0};
    XrXirInstance *instances[2]={0};
    XrXirValue retained[2]={{0},{0}};
    for(uint32_t i=0;i<2;++i) {
        traces[i].order[0]=shape.base; traces[i].order[1]=shape.left;
        traces[i].order[2]=shape.right; traces[i].order[3]=shape.root;
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.metadata_limit=UINT64_C(1)*1024*1024;
        config.value_limit=UINT64_C(1)*1024*1024;
        config.call_limit=UINT64_C(1)*1024*1024;
        config.poll_limit=UINT64_C(1000000); config.depth_limit=64;
        config.trace=state_zero_trace; config.trace_context=&traces[i];
        for(uint32_t version=4;version<=6;++version) {
            XrXirInstanceConfig stale=config; stale.abi_version=version;
            XrXirInstance *rejected=NULL;
            CHECK(xr_xir_instance_new(program,&stale,&rejected)==XR_XIR_CALL_BAD_ABI && !rejected);
        }
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY && instances[i]);
        traces[i].instance=instances[i];
        CHECK(xr_xir_instance_state(instances[i])==XR_XIR_INSTANCE_NEW);
    }
    /* Both actual instances retain code after the last producer reference. */
    xr_xir_compile_program_drop(program); program=NULL;
    CHECK(source_program_compile_live==compiler_blocks && source_program_compile_bytes==compiler_bytes);
    for(uint32_t repeat=0;repeat<24;++repeat) for(uint32_t i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],shape.advance,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult outcome;
        do { outcome=xr_xir_instance_poll_bounded(instances[i],16); }
        while(outcome.outcome.status==XR_XIR_CALL_READY);
        CHECK(outcome.outcome.status==XR_XIR_CALL_RETURNED && !outcome.outcome.wake);
        CHECK(xr_xir_panic_empty(&outcome.outcome.panic));
        CHECK(xr_xir_instance_state(instances[i])==XR_XIR_INSTANCE_READY);
        XrXirValue result={0};
        CHECK(xr_xir_instance_take_result(instances[i],&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && !result.reserved && (int64_t)result.payload==(int64_t)(41+repeat));
        for(uint32_t m=0;m<4;++m) CHECK(traces[i].begins[m]==1 && traces[i].ready[m]==1);
        CHECK(traces[i].begin_count==4 && traces[i].ready_count==4);
        CHECK(traces[i].published==1 && !traces[i].released && traces[i].busy==9);
        if(repeat==23) retained[i]=result; else xr_xir_value_drop(&result);
    }
    for(uint32_t i=0;i<2;++i) {
        size_t blocks=runtime_live,bytes=runtime_bytes;
        CHECK(blocks && bytes && source_program_compile_live==compiler_blocks &&
            source_program_compile_bytes==compiler_bytes);
        CHECK(xr_xir_instance_stop(instances[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_state(instances[i])==XR_XIR_INSTANCE_DRAINING);
        CHECK(xr_xir_instance_start(instances[i],shape.advance,NULL,0)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_resume(instances[i],0,0)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_stop(instances[i])==XR_XIR_CALL_READY);
        CHECK(runtime_live==blocks && runtime_bytes==bytes && !traces[i].released);
        CHECK(source_program_compile_live==compiler_blocks && source_program_compile_bytes==compiler_bytes);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
        instances[i]=NULL; traces[i].instance=NULL;
        CHECK(traces[i].released==1 && traces[i].busy==10);
        if(!i) CHECK(runtime_live && runtime_bytes && source_program_compile_live==compiler_blocks &&
            source_program_compile_bytes==compiler_bytes);
    }
    for(uint32_t i=0;i<2;++i) {
        CHECK(retained[i].type==XR_XIR_I64 && !retained[i].reserved && (int64_t)retained[i].payload==64);
        xr_xir_value_drop(&retained[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("numeric diamond: independent 2x24 golden, once-only init, ABI refusal, drain admission and code lifetime");
}
#endif // XIR_MODULE_STATE_ZERO_CASES_H
