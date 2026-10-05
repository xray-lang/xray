/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_dependency_execution.h - Dependency-ready source execution qualification
 *
 * KEY CONCEPT:
 *   Owned Checked artifacts and decoded packets share explicit runtime expectations.
 */
#include "xir/xxir_array.h"
typedef struct ReadyEntries {uint32_t answer,retained,values;} ReadyEntries;
static inline ReadyEntries ready_select(const XrXirModule *module){
 const char *names[]={"answer","retained","values"};uint32_t ids[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};
 for(uint32_t f=0;f<module->function_count;++f)for(unsigned n=0;n<3;++n){const XrXirFunction *fn=&module->functions[f];
  if(fn->name_length==strlen(names[n])&&!memcmp(fn->name,names[n],fn->name_length)){CHECK(ids[n]==UINT32_MAX&&!fn->parameter_count);ids[n]=f;}}
 CHECK(ids[0]!=UINT32_MAX&&ids[1]!=UINT32_MAX&&ids[2]!=UINT32_MAX);return (ReadyEntries){ids[0],ids[1],ids[2]};
}
static XrXirCallStatus ready_call(XrXirInstance *instance,uint32_t function,XrXirValue *value){
 XrXirCallStatus status=xr_xir_instance_start(instance,function,NULL,0);

 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;

 if(status==XR_XIR_CALL_RETURNED)status=xr_xir_instance_take_result(instance,value);
return status;

}
static void ready_pair(XrXirProgram *program,ReadyEntries entries,XrXirValue held[2][2]){
 XrXirInstance *instances[2]={NULL,NULL};
XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);

 for(unsigned i=0;
i<2;
++i)CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);

 for(unsigned i=0;
i<2;
++i){XrXirValue number={0};
CHECK(ready_call(instances[i],entries.answer,&number)==XR_XIR_CALL_RETURNED);
CHECK(number.type==XR_XIR_I64&&number.payload==41);
xr_xir_value_drop(&number);

 CHECK(ready_call(instances[i],entries.retained,&held[i][0])==XR_XIR_CALL_RETURNED);
CHECK(ready_call(instances[i],entries.values,&held[i][1])==XR_XIR_CALL_RETURNED);
}
 for(unsigned i=0;
i<2;
++i)CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);

}
static void ready_retained(XrXirValue held[2][2]){
 for(unsigned i=0;
i<2;
++i){const char *text=NULL;
size_t length=0;
CHECK(xr_xir_string_view(&held[i][0],&text,&length)&&length==11&&!memcmp(text,"ready-owned",11));

 XrXirValue extracted={0};
XrXirDomain *receiving=NULL;
CHECK(xr_xir_domain_new(65536,&receiving)==XR_XIR_VALUE_OK);
XrXirValueAdmission admission={xr_xir_value_arena(&held[i][1]),receiving,NULL,NULL,10000,65536};
int64_t count=0;CHECK(xr_xir_array_len(&held[i][1],&admission,&count)==XR_XIR_VALUE_OK&&count==2);
XrXirFaultDetail fault={0};
CHECK(xr_xir_array_get(&held[i][1],0,&admission,&extracted,&fault)==XR_XIR_VALUE_OK);

 XrXirValue second={0};CHECK(xr_xir_array_get(&held[i][1],1,&admission,&second,&fault)==XR_XIR_VALUE_OK);
xr_xir_value_drop(&held[i][0]);
xr_xir_value_drop(&held[i][1]);
CHECK(xr_xir_string_view(&extracted,&text,&length)&&length==11&&!memcmp(text,"ready-owned",11));
CHECK(xr_xir_string_view(&second,&text,&length)&&length==12&&!memcmp(text,"second-owned",12));xr_xir_value_drop(&second);
xr_xir_value_drop(&extracted);
xr_xir_domain_drop(receiving);
}
}
static void ready_runtime_faults(XrXirProgram *program,ReadyEntries entries){size_t baseline=runtime_live,bytes=runtime_bytes,sites=0;

 for(size_t pass=0;
pass<=sites;
++pass){runtime_attempts=0;
runtime_fail_at=pass?pass-1:SIZE_MAX;
XrXirInstance *instance=NULL;
XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
XrXirValue value={0};

 XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);

 const uint32_t functions[]={entries.answer,entries.retained,entries.values};
 for(unsigned f=0;f<3&&status==XR_XIR_CALL_READY;++f){status=ready_call(instance,functions[f],&value);
if(status==XR_XIR_CALL_RETURNED){if(f==0)CHECK(value.type==XR_XIR_I64&&value.payload==41);
xr_xir_value_drop(&value);
if(f<2)status=XR_XIR_CALL_READY;
}}
 if(!pass){CHECK(status==XR_XIR_CALL_RETURNED);
sites=runtime_attempts;
CHECK(sites);
}else CHECK(status==XR_XIR_CALL_OOM);

 if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
xr_xir_value_drop(&value);
CHECK(runtime_live==baseline&&runtime_bytes==bytes);
}
 runtime_fail_at=SIZE_MAX;
printf("ready runtime %zu OOM sites physical baseline restored\n",sites);

}
