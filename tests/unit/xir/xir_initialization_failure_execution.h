/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_initialization_failure_execution.h - Independent sticky-failure expectations
 *
 * KEY CONCEPT:
 *   Each consumer proves its own trace and owning results; agreement alone is insufficient.
 */
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_type_arena.h"
typedef struct InitModules {uint32_t root,base,failing,prelude;} InitModules;
static InitModules init_modules;
static void init_module_roles(const XrXirDeclarations *declarations){
 const char *names[]={"module-id-v1:kind=6:script:namespace=0::path=7:root.xr",
 "module-id-v1:kind=6:script:namespace=0::path=7:base.xr",
 "module-id-v1:kind=6:script:namespace=0::path=10:failing.xr",
 "stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def"};
 uint32_t ids[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};CHECK(declarations->module_count==4);
 for(uint32_t m=0;m<declarations->module_count;++m)for(unsigned role=0;role<4;++role){const XrXirSourceModule *decl=&declarations->modules[m];
  if(decl->name_length==strlen(names[role])&&!memcmp(decl->name,names[role],decl->name_length)){CHECK(ids[role]==UINT32_MAX);ids[role]=m;}}
 for(unsigned role=0;role<4;++role)CHECK(ids[role]!=UINT32_MAX);
 CHECK(declarations->root_module==ids[0]);init_modules=(InitModules){ids[0],ids[1],ids[2],ids[3]};
}
typedef struct InitTrace {uint32_t output,begins,ready,published,released,stack[16],module_begins[4],module_ready[4];bool reject;} InitTrace;
static void init_ready_prefix(const InitTrace *trace){
 CHECK(trace->module_begins[init_modules.root]==0&&trace->module_ready[init_modules.root]==0);
 CHECK(trace->module_begins[init_modules.base]==1&&trace->module_ready[init_modules.base]==1);
 CHECK(trace->module_begins[init_modules.failing]==1&&trace->module_ready[init_modules.failing]==0);
 CHECK(trace->module_begins[init_modules.prelude]==1&&trace->module_ready[init_modules.prelude]==1);
}
static XrXirOutputStatus init_output(void *context,const XrXirOutputGroup *group){
    InitTrace *trace=context;const char *text=NULL;size_t size=0;
    CHECK(group->stream==XR_XIR_STDOUT&&group->line&&group->count==1&&trace->output<2);
    CHECK(xr_xir_string_view(&group->values[0],&text,&size)&&size==1&&text[0]==(trace->output?'B':'A'));
    ++trace->output;return (!trace->reject) ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
}
static void init_trace(void *context,XrXirLifecycleEvent event,uint32_t index){
    InitTrace *trace=context;
    if(event==XR_XIR_MODULE_BEGIN){CHECK(index<4);++trace->begins;++trace->module_begins[index];}
    else if(event==XR_XIR_MODULE_READY){CHECK(index<4);++trace->ready;++trace->module_ready[index];}
    else if(event==XR_XIR_SLOT_PUBLISHED){CHECK(trace->published<16);trace->stack[trace->published++]=index;}
    else {CHECK(event==XR_XIR_SLOT_RELEASED&&trace->released<trace->published);
        CHECK(index==trace->stack[trace->published-1-trace->released++]);}
}
static void init_failure_value(const XrXirValue *error){
    XrXirValue concrete=*error;
    CHECK(xr_xir_type_is_enum(xr_xir_compile_type_arena_types(xr_xir_value_arena(error)),(XrXirType)error->type));
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(error),reader,NULL,NULL,10000,65536};
    XrXirValue text={0},count={0};
    CHECK(xr_xir_enum_get(&concrete,0,0,&admission,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_get(&concrete,0,1,&admission,&count)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t length=0;const char expected[]="initialization-owned-long-payload-after-owner-destruction";
    CHECK(xr_xir_string_view(&text,&bytes,&length)&&length==sizeof(expected)-1&&!memcmp(bytes,expected,length));
    CHECK(count.type==XR_XIR_I64&&count.payload==41);xr_xir_value_drop(&text);xr_xir_value_drop(&count);xr_xir_domain_drop(reader);
}
static void init_pair(XrXirProgram *program,uint32_t entry,XrXirValue held[3]){
    XrXirInstance *instances[2]={0};InitTrace traces[2]={{0}};XrXirInstanceResult paused[2];
    for(unsigned i=0;i<2;++i){XrXirInstanceConfig c; CHECK(xr_xir_instance_config_init(&c, sizeof(c)) == XR_XIR_CALL_READY);c.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, init_output, &traces[i]};c.trace=init_trace;c.trace_context=&traces[i];
        CHECK(xr_xir_instance_new(program,&c,&instances[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
        paused[i]=xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);CHECK(paused[i].outcome.status==XR_XIR_CALL_SUSPENDED);
        fprintf(stderr,"init observed instance%u output%u begins%u ready%u published%u released%u\n",i,traces[i].output,traces[i].begins,traces[i].ready,traces[i].published,traces[i].released);
        CHECK(traces[i].output==2&&traces[i].begins==3&&traces[i].ready==2&&!traces[i].released);init_ready_prefix(&traces[i]);
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_BUSY);
    }
    for(unsigned i=0;i<2;++i){CHECK(xr_xir_instance_resume(instances[i],paused[i].epoch,paused[i].outcome.wake)==XR_XIR_CALL_READY);
        XrXirInstanceResult failed=xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);CHECK(failed.outcome.status==XR_XIR_CALL_THROWN);init_failure_value(&failed.outcome.value);
        CHECK(traces[i].released==traces[i].published&&traces[i].published==3&&traces[i].output==2&&traces[i].ready==2);init_ready_prefix(&traces[i]);
        if(!i)CHECK(xr_xir_instance_state(instances[1])==XR_XIR_INSTANCE_INITIALIZING&&!traces[1].released);
        for(unsigned n=0;n<3;++n){CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_THROWN);
            CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status==XR_XIR_CALL_THROWN&&traces[i].output==2);}
        XrXirValue no_take={XR_XIR_I64,0,73};CHECK(xr_xir_instance_take_result(instances[i],&no_take)==XR_XIR_CALL_BAD_STATE&&no_take.type==XR_XIR_I64&&no_take.payload==73);
        XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(instances[i],&copy)==XR_XIR_CALL_THROWN);held[i]=copy.value;
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    /* A new instance retries; none of the two failed instances is revived. */
    InitTrace trace={0};XrXirInstanceConfig c; CHECK(xr_xir_instance_config_init(&c, sizeof(c)) == XR_XIR_CALL_READY);c.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, init_output, &trace};c.trace=init_trace;c.trace_context=&trace;
    XrXirInstance *retry=NULL;CHECK(xr_xir_instance_new(program,&c,&retry)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(retry,entry,NULL,0)==XR_XIR_CALL_READY);XrXirInstanceResult r=xr_xir_instance_poll_bounded(retry, UINT64_MAX);
    CHECK(r.outcome.status==XR_XIR_CALL_SUSPENDED);CHECK(xr_xir_instance_resume(retry,r.epoch,r.outcome.wake)==XR_XIR_CALL_READY);
    r=xr_xir_instance_poll_bounded(retry, UINT64_MAX);CHECK(r.outcome.status==XR_XIR_CALL_THROWN);init_failure_value(&r.outcome.value);
    XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(retry,&copy)==XR_XIR_CALL_THROWN);held[2]=copy.value;
    CHECK(trace.output==2&&trace.released==trace.published&&xr_xir_instance_free(retry)==XR_XIR_CALL_READY);
}
static void init_retained(XrXirValue held[3]){for(unsigned i=0;i<3;++i){init_failure_value(&held[i]);xr_xir_value_drop(&held[i]);}}
static void init_protocol_failures(XrXirProgram *program,uint32_t entry){
    for(unsigned mode=0;mode<2;++mode){InitTrace trace={0};trace.reject=mode!=0;
        XrXirInstanceConfig c; CHECK(xr_xir_instance_config_init(&c, sizeof(c)) == XR_XIR_CALL_READY);c.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, init_output, &trace};c.trace=init_trace;c.trace_context=&trace;
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&c,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);XrXirInstanceResult r=xr_xir_instance_poll_bounded(instance, UINT64_MAX);
        if(!mode){CHECK(r.outcome.status==XR_XIR_CALL_SUSPENDED);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_resume(instance,r.epoch,r.outcome.wake)==XR_XIR_CALL_BAD_STATE);r=xr_xir_instance_poll_bounded(instance, UINT64_MAX);}
        XrXirCallStatus expected=mode?XR_XIR_CALL_OUTPUT_ERROR:XR_XIR_CALL_CANCELLED;
        CHECK(r.outcome.status==expected&&trace.output==(mode?1u:2u)&&trace.released==trace.published);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==expected);XrXirCallResult copy={0};
        XrXirValue no_take={XR_XIR_I64,0,73};CHECK(xr_xir_instance_take_result(instance,&no_take)==XR_XIR_CALL_BAD_STATE&&no_take.type==XR_XIR_I64&&no_take.payload==73);
        CHECK(xr_xir_instance_copy_failure(instance,&copy)==expected&&!copy.value.type&&xr_xir_fault_empty(copy.panic.detail));
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
}
static void init_runtime_faults(XrXirProgram *program,uint32_t entry){
    size_t live_before=runtime_live,bytes_before=runtime_bytes,sites=0;
    for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
        InitTrace trace={0};XrXirInstanceConfig c; CHECK(xr_xir_instance_config_init(&c, sizeof(c)) == XR_XIR_CALL_READY);c.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, init_output, &trace};c.trace=init_trace;c.trace_context=&trace;
        XrXirInstance *instance=NULL;XrXirCallResult held={0};XrXirCallStatus status=xr_xir_instance_new(program,&c,&instance);
        if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
        if(status==XR_XIR_CALL_READY){XrXirInstanceResult r=xr_xir_instance_poll_bounded(instance, UINT64_MAX);status=r.outcome.status;
            if(status==XR_XIR_CALL_SUSPENDED){CHECK(xr_xir_instance_resume(instance,r.epoch,r.outcome.wake)==XR_XIR_CALL_READY);status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;}}
        if(status==XR_XIR_CALL_THROWN)status=xr_xir_instance_copy_failure(instance,&held);
        if(!pass){CHECK(status==XR_XIR_CALL_THROWN);sites=runtime_attempts;CHECK(sites);}
        else {if(status!=XR_XIR_CALL_OOM)fprintf(stderr,"init runtime failure %zu/%zu status %u\n",pass-1,sites,status);CHECK(status==XR_XIR_CALL_OOM);}
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&held.value);
        CHECK(runtime_live==live_before&&runtime_bytes==bytes_before);
    }
    runtime_fail_at=SIZE_MAX;printf("initialization runtime %zu OOM sites, physical baseline restored\n",sites);
}
