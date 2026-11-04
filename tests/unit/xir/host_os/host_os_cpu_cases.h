/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_cpu_cases.h - Real per-child CPU and affinity references
 */
#ifndef HOST_OS_CPU_CASES_H
#define HOST_OS_CPU_CASES_H
static void *host_count_thread(void *argument){
 unsigned int expected=*(unsigned int *)argument;
 for(unsigned int n=0;n<2000;++n)CHECK(xr_os_cpu_count()==expected);
 return NULL;
}
static void host_cpu_concurrency(void){
#if defined(XR_OS_WINDOWS)
 SYSTEM_INFO info;GetSystemInfo(&info);unsigned int expected=info.dwNumberOfProcessors?info.dwNumberOfProcessors:1u;
#else
 long online=sysconf(_SC_NPROCESSORS_ONLN);unsigned int expected=online>0?(unsigned int)online:1u;
#endif
 xr_thread_t workers[16];
 for(unsigned int n=0;n<16;++n)CHECK(xr_thread_create(&workers[n],host_count_thread,&expected));
 for(unsigned int n=0;n<16;++n){void *result=NULL;CHECK(!xr_thread_join(workers[n],&result)&&!result);}
 CHECK(xr_os_cpu_count()==expected);
}
static void host_cpu_execute(XrXirProgram *program,const uint32_t ids[2],bool pin){
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;host_mode=HOST_NORMAL;
 XrXirValue value={0};
 if(!pin){
  unsigned int before=count_calls;value=host_invoke(instance,ids[1],NULL,0);
  CHECK(value.type==XR_XIR_I64&&value.payload==(int64_t)xr_os_cpu_count()&&count_calls==before+1);xr_xir_value_drop(&value);
 }else{
  const int64_t inputs[]={-1,0,INT64_MAX,UINT32_MAX};
#if defined(XR_OS_WINDOWS)
  GROUP_AFFINITY original;CHECK(GetThreadGroupAffinity(GetCurrentThread(),&original));
#endif
  for(unsigned int n=0;n<sizeof(inputs)/sizeof(*inputs);++n){
   int64_t input=inputs[n];bool expected=false;
   if(input>=0){unsigned int index=(unsigned int)((uint64_t)input%xr_os_cpu_count());expected=xr_thread_pin_to_cpu(index)==0;
#if defined(XR_OS_WINDOWS)
    CHECK(SetThreadGroupAffinity(GetCurrentThread(),&original,NULL));
#endif
   }
   unsigned int before=pin_calls;XrXirValue argument={XR_XIR_I64,0,input};value=host_invoke(instance,ids[1],&argument,1);
   CHECK(value.type==XR_XIR_BOOL&&value.payload==(expected?1:0)&&pin_calls==before+(input>=0));
   if(input>=0)CHECK(last_cpu==(unsigned int)((uint64_t)input%xr_os_cpu_count()));
#if defined(XR_OS_WINDOWS)
   GROUP_AFFINITY actual;CHECK(GetThreadGroupAffinity(GetCurrentThread(),&actual));
   if(expected)CHECK(actual.Mask==((KAFFINITY)1<<last_cpu));
   CHECK(SetThreadGroupAffinity(GetCurrentThread(),&original,NULL));
#endif
   xr_xir_value_drop(&value);
  }
  unsigned int before=pin_calls;XrXirValue argument={XR_XIR_I64,0,0};host_mode=HOST_CANCEL_COUNT;
  CHECK(xr_xir_instance_start(instance,ids[1],&argument,1)==XR_XIR_CALL_READY&&host_poll(instance)==XR_XIR_CALL_CANCELLED);
  CHECK(host_mode==HOST_NORMAL&&pin_calls==before&&!value.type&&!value.payload);
 }
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // HOST_OS_CPU_CASES_H
