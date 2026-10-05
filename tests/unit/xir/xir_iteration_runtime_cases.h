/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_iteration_runtime_cases.h - Shared independent iteration expectations
 *
 * KEY CONCEPT:
 *   Each consumer checks fixed results and physical ownership with its own Program.
 */
#ifndef XIR_ITERATION_RUNTIME_CASES_H
#define XIR_ITERATION_RUNTIME_CASES_H
typedef struct IterationEntries { uint32_t answer,retained,suspended; } IterationEntries;
static IterationEntries iteration_selected;
static inline IterationEntries iteration_entries(const XrXirModule *module) {
 IterationEntries found={UINT32_MAX,UINT32_MAX,UINT32_MAX};
 const char *names[]={"answer","retained","suspended"};
 uint32_t *ids[]={&found.answer,&found.retained,&found.suspended};
 const XrXirType results[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_I64};
 for(unsigned n=0;n<3;++n)for(uint32_t f=0;f<module->function_count;++f){
  const XrXirFunction *fn=&module->functions[f];
  if(fn->name_length!=strlen(names[n])||memcmp(fn->name,names[n],fn->name_length))continue;
  C(*ids[n]==UINT32_MAX&&module->declarations->functions[f].exported);
  C(!fn->parameter_count&&fn->result==results[n]);*ids[n]=f;
 }
 C(found.answer!=UINT32_MAX&&found.retained!=UINT32_MAX&&found.suspended!=UINT32_MAX);
 return found;
}
typedef struct IterationTrace {unsigned count;} IterationTrace;
static XrXirOutputStatus iteration_trace(void *context,const XrXirOutputGroup *group){
 IterationTrace *trace=context;const int64_t expected[]={10,11,20};
 C(group->count==1 && trace->count<3 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==expected[trace->count]);++trace->count;return XR_XIR_OUTPUT_OK;
}
static void iteration_runtime_faults(XrXirProgram *program) {
 size_t base=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirValue value={0};
 XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,iteration_selected.answer,NULL,0);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
 if(status==XR_XIR_CALL_RETURNED){C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);C(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);
 status=xr_xir_instance_start(instance,iteration_selected.retained,NULL,0);if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
 if(status==XR_XIR_CALL_RETURNED)C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);}
 if(!pass){C(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;C(sites>0);}else C(status==XR_XIR_CALL_OOM);
 if(instance)C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
 C(runtime_live==base && runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("iteration runtime OOM sites %zu physical baseline restored\n",sites);
}
#include "xir_iteration_exit_allocations.h"
#endif
