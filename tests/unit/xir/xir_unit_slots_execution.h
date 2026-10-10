/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_unit_slots_execution.h - Independent zero-size local runtime expectations
 *
 * KEY CONCEPT:
 *   Runtime order and ownership are checked independently in every consumer.
 */
typedef struct UnitEntries {uint32_t answer,text;} UnitEntries;
typedef struct UnitTrace {uint32_t digits,count,last;} UnitTrace;
static UnitEntries unit_entries(const XrXirModule *module){
 UnitEntries entries={UINT32_MAX,UINT32_MAX};
 for(uint32_t f=0;f<module->function_count;++f){const XrXirFunction *fn=&module->functions[f];
  if(!module->declarations->functions[f].exported)continue;
  if(fn->name_length==6&&!memcmp(fn->name,"answer",6)){CHECK(entries.answer==UINT32_MAX);entries.answer=f;}
  if(fn->name_length==4&&!memcmp(fn->name,"text",4)){CHECK(entries.text==UINT32_MAX);entries.text=f;}
 }
 CHECK(entries.answer!=UINT32_MAX&&entries.text!=UINT32_MAX);return entries;
}
static XrXirOutputStatus unit_output(void *context,const XrXirOutputGroup *group){
 UnitTrace *trace=context;CHECK(group->stream==XR_XIR_STDOUT&&group->line&&group->count==1);
 const XrXirValue *v=&group->values[0];CHECK(v->type==XR_XIR_I64&&v->payload>=1&&v->payload<=4);
 CHECK((uint32_t)v->payload>trace->last);trace->last=(uint32_t)v->payload;
 trace->digits=trace->digits*10+trace->last;++trace->count;return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceConfig unit_config(UnitTrace *trace){
 XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);config.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, unit_output, trace};return config;
}
static void unit_retained(XrXirValue held[2]){
 static const char expected[]="unit-owned-long-string-survives-instance-and-program-destruction";
 for(unsigned i=0;i<2;++i){const char *text=NULL;size_t length=0;
  CHECK(xr_xir_string_view(&held[i],&text,&length)&&length==sizeof(expected)-1&&!memcmp(text,expected,length));
  xr_xir_value_drop(&held[i]);
 }
}
static void unit_pair(XrXirProgram *program,UnitEntries entries,XrXirValue held[2]){
 XrXirInstance *instances[2]={NULL,NULL};UnitTrace traces[2]={{0},{0}};XrXirInstanceResult suspended[2];
 for(unsigned i=0;i<2;++i){XrXirInstanceConfig config=unit_config(&traces[i]);
  CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
  CHECK(xr_xir_instance_start(instances[i],entries.answer,NULL,0)==XR_XIR_CALL_READY);
  suspended[i]=xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);CHECK(suspended[i].outcome.status==XR_XIR_CALL_SUSPENDED);
  CHECK(traces[i].digits==123&&traces[i].count==3);
 }
 for(unsigned i=0;i<2;++i){CHECK(xr_xir_instance_resume(instances[i],suspended[i].epoch,suspended[i].outcome.wake)==XR_XIR_CALL_READY);
  CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
  XrXirValue value={0};CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED);
  CHECK(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);
  CHECK(traces[i].digits==1234&&traces[i].count==4);
  if(!i)CHECK(traces[1].digits==123);
  CHECK(xr_xir_instance_start(instances[i],entries.text,NULL,0)==XR_XIR_CALL_READY);
  CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
  CHECK(xr_xir_instance_take_result(instances[i],&held[i])==XR_XIR_CALL_RETURNED);
  CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
 }
}
static void unit_cancel(XrXirProgram *program,UnitEntries entries){
 size_t live=runtime_live,bytes=runtime_bytes;UnitTrace trace={0};XrXirInstanceConfig config=unit_config(&trace);XrXirInstance *instance=NULL;
 CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 CHECK(xr_xir_instance_start(instance,entries.answer,NULL,0)==XR_XIR_CALL_READY);
 XrXirInstanceResult suspended=xr_xir_instance_poll_bounded(instance, UINT64_MAX);CHECK(suspended.outcome.status==XR_XIR_CALL_SUSPENDED&&trace.digits==123);
 CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);CHECK(trace.digits==1234&&trace.count==4);
 CHECK(xr_xir_instance_resume(instance,suspended.epoch,suspended.outcome.wake)==XR_XIR_CALL_BAD_STATE);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
/* This fixture has no child task: only an authenticated current root
 * can accept a runtime OOM. A preparation or failure-copy OOM does not. */
static void unit_free_fault_epoch(XrXirInstance *instance,XrXirCallStatus observed) {
    CHECK(xr_xir_task_executor_root_idle(instance->executor));
    CHECK(!instance->call || !instance->call->top);
    XrXirCallStatus accepted=XR_XIR_CALL_READY;
    if(instance->call && instance->call->executor_owner) {
        const XrXirCall *call=instance->call;
        const XrXirExecutorBinding binding={call->executor_owner,call->executor_activation,
            call->executor_generation,call->executor_ticket};
        CHECK(binding.owner==instance->executor);
        CHECK(xr_xir_call_driver_failure(call,&binding,&accepted));
    }
    if(accepted!=XR_XIR_CALL_READY) CHECK(accepted==XR_XIR_CALL_OOM && observed==accepted);
    XrXirCallStatus completion=xr_xir_task_executor_completion_status(instance->executor);
    CHECK(completion==accepted);
    size_t attempts=runtime_attempts;uint64_t epoch=instance->epoch;
    XrXirCallStatus freed=xr_xir_instance_free(instance);
    fprintf(stderr,"UNIT_FAULT_FREE epoch=%llu observed=%u accepted=%u completion=%u free=%u\n",
        (unsigned long long)epoch,observed,accepted,completion,freed);
    CHECK(freed==accepted && runtime_attempts==attempts);
}
static void unit_faults(XrXirProgram *program,UnitEntries entries){
 size_t live=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
  UnitTrace trace={0};XrXirInstanceConfig config=unit_config(&trace);XrXirInstance *instance=NULL;XrXirValue value={0};
  XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
  if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entries.answer,NULL,0);
  if(status==XR_XIR_CALL_READY){XrXirInstanceResult step=xr_xir_instance_poll_bounded(instance, UINT64_MAX);status=step.outcome.status;
   if(status==XR_XIR_CALL_SUSPENDED){CHECK(xr_xir_instance_resume(instance,step.epoch,step.outcome.wake)==XR_XIR_CALL_READY);status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;}}
  if(status==XR_XIR_CALL_RETURNED){status=xr_xir_instance_take_result(instance,&value);CHECK(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);}
  if(status==XR_XIR_CALL_RETURNED){status=xr_xir_instance_start(instance,entries.text,NULL,0);
   if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
   if(status==XR_XIR_CALL_RETURNED)status=xr_xir_instance_take_result(instance,&value);}
  if(!pass){CHECK(status==XR_XIR_CALL_RETURNED&&trace.digits==1234);sites=runtime_attempts;CHECK(sites);}
  else CHECK(status==XR_XIR_CALL_OOM);
  if(instance)unit_free_fault_epoch(instance,status);xr_xir_value_drop(&value);
  CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("Unit runtime %zu OOM sites; physical baseline restored\n",sites);
}
