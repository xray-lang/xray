/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_one_cases.h - Whole String state and physical release oracles
 *
 * KEY CONCEPT:
 *   Immutable String owners retain real values across exchange, faults and retirement.
 */
#ifndef XIR_MODULE_STATE_ONE_CASES_H
#define XIR_MODULE_STATE_ONE_CASES_H
#include "xir/xxir_output.h"
#include "xir/xxir_panic.h"
typedef struct StateOneShape { uint32_t advance,snapshot,read_left,read_right,root,base,left,right,slot,library_slot; } StateOneShape;
typedef struct StateOneTrace {
    XrXirInstance *instance; XrXirOutputSink sink;
    char output[512]; size_t size;
    StateOneShape shape;
    uint32_t begins[4],ready[4],published[2],released[2],busy;
} StateOneTrace;
static StateOneShape state_one_shape(const XrXirDeclarations *d,uint32_t advance,uint32_t snapshot,uint32_t read_left,uint32_t read_right) {
    CHECK(d && d->module_count==4 && d->slot_count==2 && d->root_module<4);
    uint32_t root=d->root_module;
    CHECK(d->modules[root].dependency_count==2 && d->modules[root].dependencies);
    uint32_t left=d->modules[root].dependencies[0],right=d->modules[root].dependencies[1];
    CHECK(left<4 && right<4 && left!=right && left!=root && right!=root);
    CHECK(d->modules[left].dependency_count==1 && d->modules[right].dependency_count==1);
    uint32_t base=d->modules[left].dependencies[0];
    CHECK(base<4 && base!=root && base!=left && base!=right);
    CHECK(d->modules[right].dependencies[0]==base && !d->modules[base].dependency_count);
    uint32_t slot=UINT32_MAX,library_slot=UINT32_MAX;
    for(uint32_t i=0;i<d->slot_count;++i) {
        CHECK(d->slots[i].type==XR_XIR_STRING);
        if(d->slots[i].module==root) {CHECK(slot==UINT32_MAX && d->slots[i].mutable);slot=i;}
        else {CHECK(d->slots[i].module==base && !d->slots[i].mutable && library_slot==UINT32_MAX);library_slot=i;}
    }
    CHECK(slot!=UINT32_MAX && library_slot!=UINT32_MAX);
    uint32_t entries[4]={advance,snapshot,read_left,read_right};
    for(uint32_t i=0;i<4;++i)CHECK(d->functions[entries[i]].module==root && d->functions[entries[i]].exported);
    return (StateOneShape){advance,snapshot,read_left,read_right,root,base,left,right,slot,library_slot};
}
static XrXirOutputStatus state_one_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    StateOneTrace *trace=context;
    CHECK(trace && stream==XR_XIR_STDOUT && length<=sizeof(trace->output)-trace->size);
    memcpy(trace->output+trace->size,bytes,length);trace->size+=length;return XR_XIR_OUTPUT_OK;
}
static void state_one_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    StateOneTrace *trace=context;CHECK(trace && trace->instance);
    CHECK(xr_xir_instance_free(trace->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_start(trace->instance,UINT32_MAX,NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(trace->instance)==XR_XIR_CALL_BUSY);++trace->busy;
    StateOneShape shape=trace->shape;
    if(event==XR_XIR_MODULE_BEGIN) {
        CHECK(index<4 && !trace->begins[index]);
        if(index==shape.left || index==shape.right)CHECK(trace->ready[shape.base]==1);
        if(index==shape.root)CHECK(trace->ready[shape.left]==1 && trace->ready[shape.right]==1);
        ++trace->begins[index];
    } else if(event==XR_XIR_MODULE_READY) {
        CHECK(index<4 && trace->begins[index] && !trace->ready[index]);++trace->ready[index];
    } else {
        uint32_t owned=index==shape.slot?0:1;
        CHECK(index==shape.slot || index==shape.library_slot);
        if(event==XR_XIR_SLOT_PUBLISHED) {
            CHECK(!trace->published[owned]);
            if(!owned)CHECK(trace->published[1] && trace->ready[shape.base]);
            ++trace->published[owned];
        } else {
            CHECK(event==XR_XIR_SLOT_RELEASED && trace->published[owned] && !trace->released[owned]);
            if(owned && trace->published[0])CHECK(trace->released[0]==1);
            ++trace->released[owned];
        }
    }
}
static XrXirInstance *state_one_instance(XrXirProgram *program,StateOneTrace *trace) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=UINT64_C(1)*1024*1024;config.value_limit=UINT64_C(1)*1024*1024;
    config.call_limit=UINT64_C(1)*1024*1024;config.poll_limit=1000000;config.depth_limit=64;
    trace->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,state_one_bytes,trace,512};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&trace->sink};
    config.trace=state_one_trace;config.trace_context=trace;
    for(uint32_t version=4;version<=6;++version) {
        XrXirInstanceConfig stale=config;stale.abi_version=version;XrXirInstance *rejected=NULL;
        CHECK(xr_xir_instance_new(program,&stale,&rejected)==XR_XIR_CALL_BAD_ABI && !rejected);
    }
    CHECK(xr_xir_instance_new(program,&config,&trace->instance)==XR_XIR_CALL_READY && trace->instance);
    return trace->instance;
}
static XrXirCallStatus state_one_run(XrXirInstance *instance,uint32_t entry) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if(status!=XR_XIR_CALL_READY)return status;
    XrXirInstanceResult outcome;
    do {outcome=xr_xir_instance_poll_bounded(instance,16);}while(outcome.outcome.status==XR_XIR_CALL_READY);
    CHECK(!outcome.outcome.wake && xr_xir_panic_empty(&outcome.outcome.panic));
    return outcome.outcome.status;
}
static void state_one_unit(XrXirInstance *instance) {
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_UNIT && !value.reserved && !value.payload);xr_xir_value_drop(&value);
}
static void state_one_output(StateOneTrace *trace,size_t before,bool changed) {
    const char *expected=changed?"changed\n":"module-owned-text\n";size_t length=strlen(expected);
    CHECK(trace->size-before==length && !memcmp(trace->output+before,expected,length));
}
static void state_one_string(const XrXirValue *value,bool changed) {
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length));
    const char *expected=changed?"changed":"module-owned-text";size_t expected_length=strlen(expected);
    CHECK(length==expected_length && !memcmp(bytes,expected,length));
}
static XrXirValue state_one_read(XrXirInstance *instance,uint32_t entry,bool changed) {
    CHECK(state_one_run(instance,entry)==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    state_one_string(&value,changed);return value;
}
static void state_one_library(XrXirInstance *instance,StateOneShape shape) {
    XrXirValue left=state_one_read(instance,shape.read_left,false);
    XrXirValue right=state_one_read(instance,shape.read_right,false);
    CHECK(left.payload==right.payload && instance->published[shape.library_slot]);
    CHECK(left.payload==instance->slots[shape.library_slot].payload);
    xr_xir_value_drop(&right);xr_xir_value_drop(&left);
}
static bool state_one_committed(XrXirInstance *instance,uint32_t slot) {
    if(!instance->published[slot])return false;
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&instance->slots[slot],&bytes,&length));
    bool changed=length==7 && !memcmp(bytes,"changed",7);
    CHECK(changed || (length==17 && !memcmp(bytes,"module-owned-text",17)));return changed;
}
static void state_one_faults(XrXirProgram *program,StateOneShape shape) {
    StateOneTrace baseline={0};baseline.shape=shape;XrXirInstance *instance=state_one_instance(program,&baseline);
    runtime_attempts=0;CHECK(state_one_run(instance,shape.advance)==XR_XIR_CALL_RETURNED);
    size_t sites=runtime_attempts;CHECK(sites>=4 && sites<20000);state_one_unit(instance);state_one_output(&baseline,0,false);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(!runtime_live && !runtime_bytes);
    uint32_t sticky=0,recoverable=0,committed=0;
    for(size_t ordinal=0;ordinal<sites;++ordinal) {
        StateOneTrace trace={0};trace.shape=shape;instance=state_one_instance(program,&trace);
        runtime_attempts=0;runtime_fail_at=ordinal;
        XrXirCallStatus status=state_one_run(instance,shape.advance);
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if(status!=XR_XIR_CALL_OOM)fprintf(stderr,"String fault ordinal=%zu status=%u attempts=%zu\n",ordinal,status,attempts);
        CHECK(status==XR_XIR_CALL_OOM && attempts>ordinal && !trace.size);
        XrXirValue empty={0};CHECK(xr_xir_instance_take_result(instance,&empty)==XR_XIR_CALL_BAD_STATE);
        CHECK(!empty.type && !empty.reserved && !empty.payload);
        XrXirValue sentinel={XR_XIR_I64,0,91},output=sentinel;
        XrXirCallStatus occupied=(instance->state==XR_XIR_INSTANCE_FAILED || !instance->call)?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_BAD_ARGUMENT;
        CHECK(xr_xir_instance_take_result(instance,&output)==occupied);
        CHECK(output.type==sentinel.type && output.payload==sentinel.payload);
        bool failed=xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED;
        bool changed=state_one_committed(instance,shape.slot);
        /* The final allocation is the renderer buffer, strictly after SLOT_STORE.
         * Every earlier allocation precedes that transaction commit. */
        CHECK(changed==(ordinal+1==sites));
        if(failed) {
            ++sticky;CHECK(!instance->publication_count);
            CHECK(trace.published[0]==trace.released[0] && trace.published[1]==trace.released[1]);
            size_t retry_before=runtime_attempts;uint32_t publication=trace.published[0]+trace.published[1];
            CHECK(state_one_run(instance,shape.advance)==XR_XIR_CALL_OOM && !trace.size);
            CHECK(runtime_attempts==retry_before && trace.published[0]+trace.published[1]==publication);
        } else {
            ++recoverable;if(changed)++committed;
            CHECK(state_one_run(instance,shape.advance)==XR_XIR_CALL_RETURNED);state_one_unit(instance);
            state_one_output(&trace,0,changed);state_one_library(instance,shape);
        }
        CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(trace.published[0]==trace.released[0] && trace.published[1]==trace.released[1]);
        CHECK(!runtime_live && !runtime_bytes);
    }
    CHECK(sticky && recoverable && committed);
    printf("String module execution all %zu allocation faults: sticky=%u recoverable=%u committed=%u physical=0/0\n",sites,sticky,recoverable,committed);
}
static void state_one_pair(XrXirProgram *program,StateOneShape shape) {
    CHECK(program && !runtime_live && !runtime_bytes);state_one_faults(program,shape);
    size_t compiler_blocks=source_program_compile_live,compiler_bytes=source_program_compile_bytes;
    StateOneTrace traces[2]={0};XrXirInstance *instances[2]={0};XrXirValue retained[2]={{0},{0}};
    XrXirValue retained_library[2]={{0},{0}};
    for(uint32_t i=0;i<2;++i) {traces[i].shape=shape;instances[i]=state_one_instance(program,&traces[i]);}
    for(uint32_t i=0;i<2;++i) {
        XrXirValue initial=state_one_read(instances[i],shape.snapshot,false);
        CHECK(initial.payload==instances[i]->slots[shape.slot].payload &&
            initial.payload==instances[i]->slots[shape.library_slot].payload);
        state_one_library(instances[i],shape);xr_xir_value_drop(&initial);
    }
    xr_xir_compile_program_drop(program);program=NULL;
    CHECK(source_program_compile_live==compiler_blocks && source_program_compile_bytes==compiler_bytes);
    for(uint32_t repeat=0;repeat<24;++repeat)for(uint32_t i=0;i<2;++i) {
        size_t before=traces[i].size;CHECK(state_one_run(instances[i],shape.advance)==XR_XIR_CALL_RETURNED);
        state_one_unit(instances[i]);state_one_output(&traces[i],before,repeat!=0);
        for(uint32_t m=0;m<4;++m)CHECK(traces[i].begins[m]==1 && traces[i].ready[m]==1);
        CHECK(traces[i].published[0]==1 && traces[i].published[1]==1 &&
            !traces[i].released[0] && !traces[i].released[1] && traces[i].busy==10);
        state_one_library(instances[i],shape);
    }
    for(uint32_t i=0;i<2;++i) {
        retained[i]=state_one_read(instances[i],shape.snapshot,true);
        retained_library[i]=state_one_read(instances[i],shape.read_left,false);
        CHECK(retained[i].payload!=retained_library[i].payload);
        size_t blocks=runtime_live,physical_bytes=runtime_bytes;
        CHECK(xr_xir_instance_stop(instances[i])==XR_XIR_CALL_READY);
        CHECK(runtime_live==blocks && runtime_bytes==physical_bytes);
        CHECK(xr_xir_instance_start(instances[i],shape.advance,NULL,0)==XR_XIR_CALL_BAD_STATE);
        CHECK(!traces[i].released[0] && !traces[i].released[1] && source_program_compile_live==compiler_blocks);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);traces[i].instance=NULL;
        CHECK(traces[i].released[0]==1 && traces[i].released[1]==1 && traces[i].busy==12);
    }
    CHECK(retained[0].payload!=retained[1].payload && retained_library[0].payload!=retained_library[1].payload);
    for(uint32_t i=0;i<2;++i) {
        state_one_string(&retained[i],true);state_one_string(&retained_library[i],false);
        xr_xir_value_drop(&retained[i]);xr_xir_value_drop(&retained_library[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("String diamond: 2x24 fixed output, real library initialization, root exchange, stale ABI refusals, drain, four escaped owners and physical0");
}
#endif
