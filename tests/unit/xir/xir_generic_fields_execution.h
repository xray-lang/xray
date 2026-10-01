/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_fields_execution.h - Independent generic field execution expectations
 *
 * KEY CONCEPT:
 *   Two instances and retained results exercise actual specialized class carriers.
 */
#ifndef XIR_GENERIC_FIELDS_EXECUTION_H
#define XIR_GENERIC_FIELDS_EXECUTION_H
typedef struct GenericFieldEntries {uint32_t answer,text;} GenericFieldEntries;
static GenericFieldEntries generic_fields_entries(const XrXirModule *module){
 GenericFieldEntries e={UINT32_MAX,UINT32_MAX};
 for(uint32_t i=0;i<module->function_count;++i){const XrXirFunction *f=&module->functions[i];
  if(f->name_length==6&&!memcmp(f->name,"answer",6)){CHECK(e.answer==UINT32_MAX);e.answer=i;}
  if(f->name_length==8&&!memcmp(f->name,"retained",8)){CHECK(e.text==UINT32_MAX);e.text=i;}
 }
 CHECK(e.answer!=UINT32_MAX&&e.text!=UINT32_MAX);return e;
}
static XrXirCallStatus generic_fields_run(XrXirInstance *instance,GenericFieldEntries e,XrXirValue *held){
 XrXirCallStatus s=xr_xir_instance_start(instance,e.answer,NULL,0);
 if(s==XR_XIR_CALL_READY)s=xr_xir_instance_poll(instance).outcome.status;
 if(s!=XR_XIR_CALL_RETURNED)return s;
 XrXirValue number={0};CHECK(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);
 CHECK(number.type==XR_XIR_I64&&number.payload==41);xr_xir_value_drop(&number);
 s=xr_xir_instance_start(instance,e.text,NULL,0);
 if(s==XR_XIR_CALL_READY)s=xr_xir_instance_poll(instance).outcome.status;
 if(s==XR_XIR_CALL_RETURNED)CHECK(xr_xir_instance_take_result(instance,held)==XR_XIR_CALL_RETURNED);
 return s;
}
static void generic_fields_pair(XrXirProgram *program,GenericFieldEntries e,XrXirValue held[2]){
 XrXirInstance *instances[2]={NULL,NULL};XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
 for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
 for(unsigned i=0;i<2;++i)CHECK(generic_fields_run(instances[i],e,&held[i])==XR_XIR_CALL_RETURNED);
 for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
}
static void generic_fields_faults(XrXirProgram *program,GenericFieldEntries e){
 size_t live=runtime_live,bytes=runtime_bytes,points=0;
 for(size_t pass=0;pass<=points;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
  XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirValue held={0};
  XrXirCallStatus s=xr_xir_instance_new(program,&config,&instance);
  if(s==XR_XIR_CALL_READY)s=generic_fields_run(instance,e,&held);
  if(!pass){CHECK(s==XR_XIR_CALL_RETURNED);points=runtime_attempts;CHECK(points);}else CHECK(s==XR_XIR_CALL_OOM);
  if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&held);
  CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("generic fields %zu runtime OOM points, physical baseline restored\n",points);
}
static void generic_fields_retained(XrXirValue held[2]){
 for(unsigned i=0;i<2;++i){const char *bytes=NULL;size_t length=0;
  CHECK(xr_xir_string_view(&held[i],&bytes,&length)&&length==6&&!memcmp(bytes,"mapped",6));xr_xir_value_drop(&held[i]);
 }
}
#endif
